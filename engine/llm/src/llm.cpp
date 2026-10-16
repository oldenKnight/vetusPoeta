// vp::llm::Model on the vendored llama.cpp (CPU only). Every llama.cpp object lives in an RAII owner; every public
// function catches at the boundary and returns a Result. Inputs are validated before they reach llama.cpp, whose
// GGML_ASSERTs would abort the process (token counts against n_ctx, batch sizes against n_batch).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

#include "llama.h"
#include "prompts.h"
#include "vp/fs.h"
#include "vp/llm.h"
#include "vp/sha256.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace vp::llm {

namespace {

// ---------------------------------------------------------------------------------------------- logging, backend
std::mutex g_logMu;
LogSink g_sink = nullptr;
std::string g_lastError;   // last llama.cpp error line, appended to load failures

void llamaLog(ggml_log_level level, const char* text, void*) {
  if (text == nullptr || (level != GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_ERROR)) return;
  std::string line(text);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (line.empty()) return;
  std::lock_guard<std::mutex> lk(g_logMu);
  if (level == GGML_LOG_LEVEL_ERROR) g_lastError = line;
  if (g_sink) g_sink(level == GGML_LOG_LEVEL_ERROR ? 1 : 2, line.c_str());
}

std::string takeLastError() {
  std::lock_guard<std::mutex> lk(g_logMu);
  std::string e;
  e.swap(g_lastError);
  return e;
}

// llama_backend_init once per process (after the CPUID gate), llama_backend_free at exit.
class Backend {
 public:
  Backend() {
    llama_log_set(llamaLog, nullptr);
    llama_backend_init();
  }
  ~Backend() { llama_backend_free(); }
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;
};

void ensureBackend() { static Backend backend; }

int64_t steadyMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------- RAII owners
struct ModelFree {
  void operator()(llama_model* m) const {
    if (m) llama_model_free(m);
  }
};
struct ContextFree {
  void operator()(llama_context* c) const {
    if (c) llama_free(c);
  }
};
struct SamplerFree {
  void operator()(llama_sampler* s) const {
    if (s) llama_sampler_free(s);
  }
};
using ModelPtr = std::unique_ptr<llama_model, ModelFree>;
using ContextPtr = std::unique_ptr<llama_context, ContextFree>;
using SamplerPtr = std::unique_ptr<llama_sampler, SamplerFree>;

class Batch {
 public:
  explicit Batch(int capacity) : b_(llama_batch_init(capacity, 0, 1)), cap_(capacity) {}
  ~Batch() { llama_batch_free(b_); }
  Batch(const Batch&) = delete;
  Batch& operator=(const Batch&) = delete;
  llama_batch& get() { return b_; }
  int capacity() const { return cap_; }

 private:
  llama_batch b_;
  int cap_;
};

// ---------------------------------------------------------------------------------------------- file checks
constexpr uint64_t kMinModelBytes = 1ull << 20;
constexpr uint64_t kMaxModelBytes = 4ull << 30;

struct HashEntry {
  std::string path;
  uint64_t size = 0;
  int64_t mtime = 0;
  std::string sha;
};
std::mutex g_hashMu;
std::list<HashEntry> g_hashCache;   // most recent first, at most kHashCacheCap entries
constexpr size_t kHashCacheCap = 8;

Result<std::string> hashFile(const std::string& path) {
  fs::FilePtr f = fs::openFile(path, "rb");
  if (!f) return Error{ErrorCode::ModelLoadFailed, "cannot read " + path, "The model file could not be read."};
  Sha256 h;
  std::vector<unsigned char> buf(1u << 20);
  for (;;) {
    const size_t n = std::fread(buf.data(), 1, buf.size(), f.get());
    if (n) h.update(buf.data(), n);
    if (n < buf.size()) {
      if (std::ferror(f.get()))
        return Error{ErrorCode::ModelLoadFailed, "read error in " + path, "The model file could not be read."};
      break;
    }
  }
  return h.finishHex();
}

uint32_t le32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const unsigned char* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

// ---------------------------------------------------------------------------------------------- scoring helpers
double logProbOf(const float* logits, int nVocab, llama_token tok) {
  float mx = logits[0];
  for (int i = 1; i < nVocab; ++i) mx = std::max(mx, logits[i]);
  double z = 0;
  for (int i = 0; i < nVocab; ++i) z += std::exp(static_cast<double>(logits[i] - mx));
  return static_cast<double>(logits[tok] - mx) - std::log(z);
}

const char* const kNotLoadedHint = "The local model is not loaded. Try the local model test in Settings.";

}  // namespace

// ================================================================================================ free functions
void setLogSink(LogSink sink) {
  std::lock_guard<std::mutex> lk(g_logMu);
  g_sink = sink;
}

bool rerankEnabled() { return prompts::kLatinRerankGatePassed; }
bool built() { return true; }

Result<FileCheck> checkModelFile(const std::string& path, bool hash) {
  try {
    FileCheck fc;
    if (path.empty() || !fs::isRegularFile(path))
      return Error{ErrorCode::ModelMissing, "no model file at '" + path + "'",
                   "The local model file was not found. Choose it in Settings, or reinstall the local model."};
    fc.exists = true;
    Result<uint64_t> size = fs::fileSize(path);
    if (!size) return Error{ErrorCode::ModelLoadFailed, size.error().message, "The model file could not be read."};
    fc.sizeBytes = size.value();
    {
      fs::FilePtr f = fs::openFile(path, "rb");
      unsigned char head[24] = {0};
      if (!f || std::fread(head, 1, sizeof head, f.get()) != sizeof head)
        return Error{ErrorCode::ModelLoadFailed, "cannot read the header of " + path, "The model file could not be read."};
      const uint32_t version = le32(head + 4);
      const uint64_t tensors = le64(head + 8);
      if (std::memcmp(head, "GGUF", 4) != 0 || version < 2 || version > 3 || tensors == 0 || tensors > 100000)
        return Error{ErrorCode::ModelLoadFailed, "not a GGUF v2/v3 file: " + path,
                     "That file is not a model this app can use (a .gguf file is expected)."};
    }
    if (fc.sizeBytes < kMinModelBytes || fc.sizeBytes > kMaxModelBytes)
      return Error{ErrorCode::ModelLoadFailed, "implausible model size " + std::to_string(fc.sizeBytes),
                   "That file is not a model this app can use (wrong size)."};
    fc.gguf = true;
    if (fc.sizeBytes != kModelSizeBytes) return fc;   // cannot match: no need to read 400 MB
    Result<int64_t> mtr = fs::mtime(path);
    const int64_t mt = mtr.ok() ? mtr.value() : 0;
    {
      std::lock_guard<std::mutex> lk(g_hashMu);
      for (auto it = g_hashCache.begin(); it != g_hashCache.end(); ++it) {
        if (it->path == path && it->size == fc.sizeBytes && it->mtime == mt) {
          fc.sha256 = it->sha;
          g_hashCache.splice(g_hashCache.begin(), g_hashCache, it);
          fc.hashed = true;
          fc.sha256ok = fc.sha256 == kModelSha256;
          return fc;
        }
      }
    }
    if (!hash) return fc;   // not hashed yet: sha256ok stays false, hashed false
    Result<std::string> sha = hashFile(path);
    if (!sha) return sha.error();
    fc.sha256 = sha.value();
    fc.hashed = true;
    fc.sha256ok = fc.sha256 == kModelSha256;
    std::lock_guard<std::mutex> lk(g_hashMu);
    g_hashCache.push_front(HashEntry{path, fc.sizeBytes, mt, fc.sha256});
    while (g_hashCache.size() > kHashCacheCap) g_hashCache.pop_back();
    return fc;
  } catch (const std::exception& e) {
    return Error{ErrorCode::Internal, std::string("checkModelFile: ") + e.what(), "The model file could not be checked."};
  } catch (...) {
    return Error{ErrorCode::Internal, "checkModelFile: unknown error", "The model file could not be checked."};
  }
}

// ================================================================================================ Model
struct Model::Impl {
  Config cfg;
  std::string path;          // file of the last inspect() or load()
  FileCheck check;
  bool checked = false;
  ModelPtr model;
  ContextPtr ctx;
  std::unique_ptr<Batch> batch;
  const llama_vocab* vocab = nullptr;
  int nVocab = 0;
  int nCtx = 0;
  std::string chatTmpl;
  int64_t lastUse = 0;
  int lastLoadMs = 0;
  std::vector<float> saved;               // logits after the prompt (choose)
  std::vector<llama_token> toks, opt, pre;

  int64_t now() const { return cfg.clock ? cfg.clock() : steadyMs(); }
  bool loaded() const { return model && ctx && batch; }

  void free() {
    batch.reset();
    ctx.reset();
    model.reset();
    vocab = nullptr;
    nVocab = nCtx = 0;
    std::vector<float>().swap(saved);
    std::vector<llama_token>().swap(toks);
    std::vector<llama_token>().swap(opt);
    std::vector<llama_token>().swap(pre);
#if defined(__GLIBC__)
    malloc_trim(0);   // give freed arenas back so RSS returns to the pre-load level
#endif
  }

  bool tokenize(std::string_view text, bool special, std::vector<llama_token>& out) const {
    out.clear();
    if (text.empty()) return true;
    if (text.size() > 64 * 1024) return false;
    const int32_t need = -llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, false, special);
    if (need <= 0) return need == 0;
    out.resize(static_cast<size_t>(need));
    const int32_t n = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), out.data(), need, false, special);
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    return true;
  }

  // System line + one user turn, rendered with the model's own chat template, ending with the assistant header.
  bool chat(const std::string& user, std::string& out) const {
    llama_chat_message msgs[2] = {{"system", prompts::kSystem}, {"user", user.c_str()}};
    std::vector<char> buf(2 * (user.size() + std::strlen(prompts::kSystem)) + 256);
    int32_t n = llama_chat_apply_template(chatTmpl.c_str(), msgs, 2, true, buf.data(), static_cast<int32_t>(buf.size()));
    if (n < 0) return false;
    if (static_cast<size_t>(n) > buf.size()) {
      buf.resize(static_cast<size_t>(n));
      n = llama_chat_apply_template(chatTmpl.c_str(), msgs, 2, true, buf.data(), static_cast<int32_t>(buf.size()));
      if (n < 0 || static_cast<size_t>(n) > buf.size()) return false;
    }
    out.assign(buf.data(), static_cast<size_t>(n));
    return true;
  }

  void clearMemory() { llama_memory_clear(llama_get_memory(ctx.get()), false); }

  // Decodes t[0..n) at positions pos0.. in sequence 0, in chunks of the batch capacity. For each run index
  // i >= firstOut the logits are requested and onLogits(i, logits) runs right after its chunk.
  template <class F>
  bool decodeRun(const llama_token* t, int n, int pos0, int firstOut, F&& onLogits, std::string& err) {
    llama_batch& b = batch->get();
    for (int start = 0; start < n; start += batch->capacity()) {
      const int m = std::min(batch->capacity(), n - start);
      b.n_tokens = m;
      for (int j = 0; j < m; ++j) {
        b.token[j] = t[start + j];
        b.pos[j] = pos0 + start + j;
        b.n_seq_id[j] = 1;
        b.seq_id[j][0] = 0;
        b.logits[j] = (start + j) >= firstOut ? 1 : 0;
      }
      const int32_t rc = llama_decode(ctx.get(), b);
      if (rc != 0) {
        err = "llama_decode returned " + std::to_string(rc);
        return false;
      }
      for (int j = 0; j < m; ++j) {
        if (start + j < firstOut) continue;
        const float* lg = llama_get_logits_ith(ctx.get(), j);
        if (lg == nullptr) {
          err = "no logits for batch row " + std::to_string(j);
          return false;
        }
        onLogits(start + j, lg);
      }
    }
    return true;
  }
};

Model::Model() : impl_(std::make_unique<Impl>()) {}
Model::~Model() {
  if (impl_) impl_->free();
}

bool Model::loaded() const { return impl_->loaded(); }

Status Model::status() const {
  Status s;
  s.loaded = impl_->loaded();
  s.cpuOk = cpuSupported();
  s.path = impl_->path;
  s.sizeBytes = impl_->checked ? impl_->check.sizeBytes : 0;
  s.sha256ok = impl_->checked && impl_->check.sha256ok;
  s.available = impl_->checked && impl_->check.exists && impl_->check.gguf && impl_->check.sha256ok;
  s.lastLoadMs = impl_->lastLoadMs;
  return s;
}

Result<Status> Model::inspect(const std::string& path, bool hash) {
  impl_->path = path;
  impl_->checked = false;
  impl_->check = FileCheck{};
  Result<FileCheck> r = checkModelFile(path, hash);
  if (!r) return r.error();
  impl_->check = r.value();
  impl_->checked = true;
  return status();
}

void Model::touch() { impl_->lastUse = impl_->now(); }

bool Model::maybeUnload(int64_t nowMs) {
  if (!impl_->loaded()) return false;
  if (nowMs - impl_->lastUse < impl_->cfg.idleUnloadMs) return false;
  unload();
  return true;
}

void Model::unload() {
  try {
    impl_->free();
  } catch (...) {
  }
}

Result<void> Model::load(const Config& c) {
  try {
    Config cfg = c;
    const unsigned hw = std::thread::hardware_concurrency();
    const int hwMax = hw == 0 ? 2 : static_cast<int>(std::min(hw, 64u));
    cfg.threads = std::max(1, std::min({cfg.threads, kMaxThreads, hwMax}));
    cfg.ctx = std::max(128, std::min(cfg.ctx, kMaxCtx));
    if (cfg.idleUnloadMs <= 0) cfg.idleUnloadMs = kDefaultIdleUnloadMs;
    if (impl_->loaded() && impl_->path == cfg.path) {
      impl_->cfg.clock = cfg.clock;
      impl_->cfg.idleUnloadMs = cfg.idleUnloadMs;
      touch();
      return {};
    }
    unload();
    if (!cpuSupported())
      return Error{ErrorCode::ModelUnsupportedCpu, "CPU lacks AVX2/FMA/F16C/BMI2 or OS AVX support",
                   "This computer's processor lacks the AVX2 instructions the local model needs. "
                   "Everything else works without the local model."};
    Result<FileCheck> fc = checkModelFile(cfg.path, false);
    if (!fc) return fc.error();
    if (!impl_->checked || impl_->path != cfg.path) {
      impl_->path = cfg.path;
      impl_->check = fc.value();
      impl_->checked = true;
    }
    impl_->cfg = cfg;
    ensureBackend();
    takeLastError();
    const int64_t t0 = steadyMs();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;   // use_mmap = true, use_mlock = false
    mp.check_tensors = false;
    mp.progress_callback = nullptr;
    impl_->model.reset(llama_model_load_from_file(cfg.path.c_str(), mp));
    if (!impl_->model) {
      const std::string why = takeLastError();
      return Error{ErrorCode::ModelLoadFailed, "llama_model_load_from_file failed" + (why.empty() ? "" : ": " + why),
                   "The local model file could not be loaded. It may be damaged; reinstall it."};
    }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(cfg.ctx);
    cp.n_batch = kBatch;
    cp.n_ubatch = kBatch;
    cp.n_seq_max = 1;
    cp.n_threads = cfg.threads;
    cp.n_threads_batch = cfg.threads;
    cp.no_perf = true;
    cp.embeddings = false;
    cp.offload_kqv = false;
    cp.op_offload = false;
    impl_->ctx.reset(llama_init_from_model(impl_->model.get(), cp));
    if (!impl_->ctx) {
      const std::string why = takeLastError();
      impl_->free();
      return Error{ErrorCode::ModelLoadFailed, "llama_init_from_model failed" + (why.empty() ? "" : ": " + why),
                   "The local model could not start. Close other programs to free memory and try again."};
    }
    impl_->vocab = llama_model_get_vocab(impl_->model.get());
    impl_->nVocab = llama_vocab_n_tokens(impl_->vocab);
    impl_->nCtx = static_cast<int>(llama_n_ctx(impl_->ctx.get()));
    const char* tmpl = llama_model_chat_template(impl_->model.get(), nullptr);
    impl_->chatTmpl = tmpl ? tmpl : "chatml";
    impl_->batch = std::make_unique<Batch>(kBatch);
    if (impl_->nVocab <= 0 || impl_->nCtx <= 0) {
      impl_->free();
      return Error{ErrorCode::ModelLoadFailed, "model has no vocabulary or context", "The local model file is not usable."};
    }
    impl_->saved.reserve(static_cast<size_t>(impl_->nVocab));
    impl_->lastLoadMs = static_cast<int>(steadyMs() - t0);
    touch();
    return {};
  } catch (const std::exception& e) {
    unload();
    return Error{ErrorCode::ModelLoadFailed, std::string("load: ") + e.what(), "The local model could not be loaded."};
  } catch (...) {
    unload();
    return Error{ErrorCode::ModelLoadFailed, "load: unknown error", "The local model could not be loaded."};
  }
}

Result<int> Model::choose(std::string_view prompt, const std::vector<std::string>& options, std::vector<float>* scores) {
  try {
    Impl& m = *impl_;
    if (!m.loaded()) return Error{ErrorCode::ModelLoadFailed, "choose: model not loaded", kNotLoadedHint};
    if (options.empty() || options.size() > 64)
      return Error{ErrorCode::BadParams, "choose: 1..64 options required", "The question had no options."};
    for (const std::string& o : options)
      if (o.empty()) return Error{ErrorCode::BadParams, "choose: empty option", "An option was empty."};
    touch();
    std::string text;
    if (!m.chat(prompts::chooseUser(prompt, options), text))
      return Error{ErrorCode::Internal, "choose: chat template failed", "The local model could not read the question."};
    if (!m.tokenize(text, true, m.toks) || m.toks.empty())
      return Error{ErrorCode::BadParams, "choose: prompt could not be tokenised", "The question could not be read."};
    const int n = static_cast<int>(m.toks.size());
    if (n >= m.nCtx - 8)
      return Error{ErrorCode::BadParams, "choose: prompt has " + std::to_string(n) + " tokens", "The question is too long."};
    m.clearMemory();
    std::string err;
    m.saved.clear();
    if (!m.decodeRun(m.toks.data(), n, 0, n - 1,
                     [&](int, const float* lg) { m.saved.assign(lg, lg + m.nVocab); }, err))
      return Error{ErrorCode::Internal, "choose: " + err, "The local model failed on this question."};
    llama_token end = llama_vocab_eot(m.vocab);
    if (end == LLAMA_TOKEN_NULL) end = llama_vocab_eos(m.vocab);
    if (scores) scores->assign(options.size(), 0.0f);
    int best = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (size_t k = 0; k < options.size(); ++k) {
      if (!m.tokenize(options[k], false, m.opt) || m.opt.empty())
        return Error{ErrorCode::BadParams, "choose: option could not be tokenised", "An option could not be read."};
      if (end != LLAMA_TOKEN_NULL) m.opt.push_back(end);
      const int len = static_cast<int>(m.opt.size());
      if (n + len > m.nCtx)
        return Error{ErrorCode::BadParams, "choose: option too long for the context", "An option is too long."};
      double lp = logProbOf(m.saved.data(), m.nVocab, m.opt[0]);
      if (len > 1) {
        const bool ok = m.decodeRun(m.opt.data(), len - 1, n, 0,
                                    [&](int i, const float* lg) { lp += logProbOf(lg, m.nVocab, m.opt[static_cast<size_t>(i) + 1]); },
                                    err);
        llama_memory_seq_rm(llama_get_memory(m.ctx.get()), 0, n, -1);   // back to the prompt for the next option
        if (!ok) return Error{ErrorCode::Internal, "choose: " + err, "The local model failed on this question."};
      }
      const double mean = lp / len;
      if (scores) (*scores)[k] = static_cast<float>(mean);
      if (mean > bestScore) {   // strict: ties keep the lowest index
        bestScore = mean;
        best = static_cast<int>(k);
      }
    }
    touch();
    return best;
  } catch (const std::exception& e) {
    return Error{ErrorCode::Internal, std::string("choose: ") + e.what(), "The local model failed on this question."};
  } catch (...) {
    return Error{ErrorCode::Internal, "choose: unknown error", "The local model failed on this question."};
  }
}

Result<TextScore> Model::scoreText(std::string_view prefix, std::string_view continuation) {
  try {
    Impl& m = *impl_;
    if (!m.loaded()) return Error{ErrorCode::ModelLoadFailed, "scoreText: model not loaded", kNotLoadedHint};
    if (prefix.empty() || continuation.empty())
      return Error{ErrorCode::BadParams, "scoreText: empty prefix or continuation", "Nothing to score."};
    touch();
    std::string all(prefix);
    all += continuation;
    if (!m.tokenize(prefix, false, m.pre) || !m.tokenize(all, false, m.toks))
      return Error{ErrorCode::BadParams, "scoreText: text could not be tokenised", "The text could not be read."};
    const int n = static_cast<int>(m.toks.size());
    if (n > m.nCtx) return Error{ErrorCode::BadParams, "scoreText: text too long", "The text is too long."};
    // The scored part starts where the joint tokenisation leaves the prefix's own tokens (a token may straddle
    // the boundary); at least one token conditions the first scored one.
    int start = 0;
    while (start < n && start < static_cast<int>(m.pre.size()) && m.toks[static_cast<size_t>(start)] == m.pre[static_cast<size_t>(start)]) ++start;
    start = std::max(start, 1);
    if (start >= n) return Error{ErrorCode::BadParams, "scoreText: continuation has no tokens", "Nothing to score."};
    m.clearMemory();
    TextScore ts;
    std::string err;
    if (!m.decodeRun(m.toks.data(), n - 1, 0, start - 1,
                     [&](int i, const float* lg) { ts.logProb += logProbOf(lg, m.nVocab, m.toks[static_cast<size_t>(i) + 1]); },
                     err))
      return Error{ErrorCode::Internal, "scoreText: " + err, "The local model failed on this text."};
    ts.tokens = n - start;
    touch();
    return ts;
  } catch (const std::exception& e) {
    return Error{ErrorCode::Internal, std::string("scoreText: ") + e.what(), "The local model failed on this text."};
  } catch (...) {
    return Error{ErrorCode::Internal, "scoreText: unknown error", "The local model failed on this text."};
  }
}

Result<std::string> Model::simplify(std::string_view sentence, std::string_view lang) {
  try {
    Impl& m = *impl_;
    if (!m.loaded()) return Error{ErrorCode::ModelLoadFailed, "simplify: model not loaded", kNotLoadedHint};
    if (lang != "en" && lang != "es")
      return Error{ErrorCode::BadParams, "simplify: lang must be en or es", "Only English and Spanish can be simplified."};
    size_t a = 0, b = sentence.size();
    while (a < b && static_cast<unsigned char>(sentence[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(sentence[b - 1]) <= ' ') --b;
    const std::string_view s = sentence.substr(a, b - a);
    if (s.empty() || s.size() > 1000)
      return Error{ErrorCode::BadParams, "simplify: sentence empty or longer than 1000 bytes", "The sentence is empty or too long."};
    touch();
    std::string text;
    if (!m.chat(prompts::simplifyUser(s, lang == "es"), text))
      return Error{ErrorCode::Internal, "simplify: chat template failed", "The local model could not read the sentence."};
    if (!m.tokenize(text, true, m.toks) || m.toks.empty())
      return Error{ErrorCode::BadParams, "simplify: sentence could not be tokenised", "The sentence could not be read."};
    const int n = static_cast<int>(m.toks.size());
    if (n + kSimplifyMaxTokens > m.nCtx)
      return Error{ErrorCode::BadParams, "simplify: prompt has " + std::to_string(n) + " tokens", "The sentence is too long."};
    SamplerPtr chain(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (!chain) return Error{ErrorCode::Internal, "simplify: sampler chain", "The local model could not start."};
    llama_sampler* grammar = llama_sampler_init_grammar(m.vocab, prompts::kSimplifyGrammar, "root");
    if (!grammar) return Error{ErrorCode::Internal, "simplify: grammar rejected", "The local model could not start."};
    llama_sampler_chain_add(chain.get(), grammar);              // the chain owns it from here
    llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());
    m.clearMemory();
    std::string err;
    if (!m.decodeRun(m.toks.data(), n, 0, n - 1, [](int, const float*) {}, err))
      return Error{ErrorCode::Internal, "simplify: " + err, "The local model failed on this sentence."};
    std::string out;
    char piece[256];
    for (int i = 0; i < kSimplifyMaxTokens; ++i) {
      llama_token tok = llama_sampler_sample(chain.get(), m.ctx.get(), -1);
      if (tok == LLAMA_TOKEN_NULL || llama_vocab_is_eog(m.vocab, tok)) break;
      const int32_t len = llama_token_to_piece(m.vocab, tok, piece, static_cast<int32_t>(sizeof piece), 0, false);
      if (len > 0) out.append(piece, static_cast<size_t>(len));
      if (out.find('\n') != std::string::npos) break;
      if (i + 1 == kSimplifyMaxTokens) break;
      if (!m.decodeRun(&tok, 1, n + i, 0, [](int, const float*) {}, err))
        return Error{ErrorCode::Internal, "simplify: " + err, "The local model failed on this sentence."};
    }
    const size_t nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);
    size_t x = 0, y = out.size();
    while (x < y && static_cast<unsigned char>(out[x]) <= ' ') ++x;
    while (y > x && static_cast<unsigned char>(out[y - 1]) <= ' ') --y;
    touch();
    return out.substr(x, y - x);
  } catch (const std::exception& e) {
    return Error{ErrorCode::Internal, std::string("simplify: ") + e.what(), "The local model failed on this sentence."};
  } catch (...) {
    return Error{ErrorCode::Internal, "simplify: unknown error", "The local model failed on this sentence."};
  }
}

// ================================================================================================ advisors
Advisors makeAdvisors(Model& model, const Config& cfg) {
  Advisors a;
  Model* m = &model;
  a.chooseSense = [m, cfg](std::string_view question, const std::vector<std::string>& options,
                           std::vector<float>* scores) -> Result<int> {
    if (!m->loaded()) {
      Result<void> r = m->load(cfg);
      if (!r) return r.error();
    }
    return m->choose(question, options, scores);
  };
  if (rerankEnabled()) {
    a.rerankLatin = [m, cfg](const std::vector<std::string>& cands, std::vector<float>* scores) -> Result<int> {
      if (cands.empty()) return Error{ErrorCode::BadParams, "rerankLatin: no candidates", "Nothing to rank."};
      if (!m->loaded()) {
        Result<void> r = m->load(cfg);
        if (!r) return r.error();
      }
      if (scores) scores->assign(cands.size(), 0.0f);
      int best = 0;
      double bestLp = -std::numeric_limits<double>::infinity();
      for (size_t i = 0; i < cands.size(); ++i) {
        Result<TextScore> s = m->scoreText(prompts::kGatePrefix, " " + cands[i]);
        if (!s) return s.error();
        if (scores) (*scores)[i] = static_cast<float>(s->logProb);
        if (s->logProb > bestLp) {
          bestLp = s->logProb;
          best = static_cast<int>(i);
        }
      }
      return best;
    };
  }
  return a;
}

}  // namespace vp::llm
