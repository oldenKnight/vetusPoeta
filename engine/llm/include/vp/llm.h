// vp::llm — engine ii, the optional local model. [CONTRACT, DESIGN.md §11]
// One Model per engine process, used from one thread (the CLI worker). The model answers closed questions only:
// `choose` scores N given options by teacher-forced log-probability, `scoreText` scores a continuation (the
// minimal-pair gate), `simplify` writes one short English sentence under a GBNF grammar. Nothing here throws and
// nothing touches the network: the module only opens a local GGUF file that the user or the installer put there.
//
// Two builds of this header:
//  * VP_LLM_REAL (set by the `vp_llm` target when VP_WITH_LLM=ON): declarations, implemented in engine/llm/src
//    on top of the vendored llama.cpp.
//  * otherwise: a header-only stub with the same API that answers model_missing, so the CLI and the tests link
//    without llama.cpp (VP_WITH_LLM=OFF).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vp/result.h"

namespace vp::llm {

constexpr int kMaxThreads = 4;
constexpr int kMaxCtx = 512;
constexpr int kBatch = 64;                     // n_batch = n_ubatch
constexpr int kSimplifyMaxTokens = 48;
constexpr int64_t kDefaultIdleUnloadMs = 60000;
// The model file the prompts and the gate were measured with. The same values are recorded in models/README.md
// (a test keeps them equal). Another GGUF can be located by the user but reports sha256ok=false.
constexpr uint64_t kModelSizeBytes = 397808192ull;
constexpr const char* kModelSha256 = "6eb923e7d26e9cea28811e1a8e852009b21242fb157b26149d3b188f3a8c8653";

struct Config {
  std::string path;                                // the .gguf file
  int threads = kMaxThreads;                       // clamped to [1, min(4, hardware threads)]
  int ctx = kMaxCtx;                               // clamped to [128, 512]
  int64_t idleUnloadMs = kDefaultIdleUnloadMs;     // maybeUnload() frees the model after this much idle time
  int64_t (*clock)() = nullptr;                    // milliseconds; nullptr = steady clock (tests inject one)
};

struct Status {
  bool available = false;   // a model file was checked: GGUF, readable, SHA-256 equal to kModelSha256
  bool loaded = false;
  bool cpuOk = false;       // AVX2 + FMA + F16C + BMI2 with OS support for the AVX state
  std::string path;
  uint64_t sizeBytes = 0;
  bool sha256ok = false;
  int lastLoadMs = 0;
};

struct CpuFeatures {
  bool x86 = false, osAvx = false, avx = false, avx2 = false, fma = false, f16c = false, bmi2 = false;
};

struct FileCheck {
  bool exists = false, gguf = false, sha256ok = false, hashed = false;
  uint64_t sizeBytes = 0;
  std::string sha256;       // lower-case hex when hashed
};

struct TextScore {
  double logProb = 0;       // natural log, summed over the continuation's tokens
  int tokens = 0;
};

// Sink for llama.cpp's warnings and errors (level 1 = error, 2 = warning); everything else is dropped.
using LogSink = void (*)(int level, const char* text);

// Closed-question hooks handed to engine i. Each callback loads the model on first use (inside a job only) and
// returns an error the engine must treat as "no advice" (keep its own choice). `rerankLatin` is empty unless the
// minimal-pair gate passed (rerankEnabled()).
struct Advisors {
  std::function<Result<int>(std::string_view question, const std::vector<std::string>& options,
                            std::vector<float>* scores)> chooseSense;
  std::function<Result<int>(const std::vector<std::string>& latinCandidates, std::vector<float>* scores)> rerankLatin;
  explicit operator bool() const { return static_cast<bool>(chooseSense); }
};

#if defined(VP_LLM_REAL) && VP_LLM_REAL

CpuFeatures cpuFeatures();          // CPUID (cached after the first call)
bool cpuSupported();                // what load() requires before any llama.cpp call
void setLogSink(LogSink sink);
// Opens the file, checks the GGUF magic and version, the size (1 MiB .. 4 GiB) and, when `hash`, its SHA-256
// against kModelSha256 (cached per path, size and modification time, so only the first call reads the file).
// Errors: model_missing (no file), model_load_failed (not a GGUF / unreadable).
Result<FileCheck> checkModelFile(const std::string& path, bool hash);
// Latin-side reranking ships only if the gate of PREPLAN 2.2 passed (engine/llm/prompts.h records the result).
bool rerankEnabled();
bool built();                        // true in this build

class Model {
 public:
  Model();
  ~Model();
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  Status status() const;
  // Remembers the file this model will load and checks it (see checkModelFile). Does not load anything.
  Result<Status> inspect(const std::string& path, bool hash);
  // CPUID first (model_unsupported_cpu), then the file (model_missing / model_load_failed), then llama.cpp.
  // Loading the already loaded file is a no-op (touches the idle clock).
  Result<void> load(const Config& cfg);
  void unload();
  bool loaded() const;
  void touch();                       // marks use now (the config's clock)
  bool maybeUnload(int64_t nowMs);    // unloads after cfg.idleUnloadMs of idleness; true when it did

  // Closed choice: the option with the highest length-normalised log-probability as the assistant's answer to
  // `prompt` (chat template, system line and option list from prompts.h). Ties -> lowest index. `scores` gets
  // the mean log-probability per token of each option. Deterministic for a given file and thread count.
  Result<int> choose(std::string_view prompt, const std::vector<std::string>& options, std::vector<float>* scores);
  // Raw text (no chat template): log P(continuation | prefix). `prefix` must not be empty.
  Result<TextScore> scoreText(std::string_view prefix, std::string_view continuation);
  // One English sentence, greedy, at most 48 tokens, printable ASCII by grammar. lang "en" or "es" (the input's
  // language; the instruction is given in that language, the output is English).
  Result<std::string> simplify(std::string_view sentence, std::string_view lang);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Advisors bound to `model` (which must outlive them); `cfg` is used for the lazy load.
Advisors makeAdvisors(Model& model, const Config& cfg);

#else  // header-only stub: VP_WITH_LLM=OFF

namespace detail {
inline Error notBuilt() {
  return Error{ErrorCode::ModelMissing, "the local model engine is not part of this build",
               "This version of vetus poeta was built without the local model."};
}
}  // namespace detail

inline CpuFeatures cpuFeatures() { return CpuFeatures{}; }
inline bool cpuSupported() { return false; }
inline void setLogSink(LogSink) {}
inline Result<FileCheck> checkModelFile(const std::string&, bool) { return detail::notBuilt(); }
inline bool rerankEnabled() { return false; }
inline bool built() { return false; }

class Model {
 public:
  Status status() const { return Status{}; }
  Result<Status> inspect(const std::string&, bool) { return detail::notBuilt(); }
  Result<void> load(const Config&) { return detail::notBuilt(); }
  void unload() {}
  bool loaded() const { return false; }
  void touch() {}
  bool maybeUnload(int64_t) { return false; }
  Result<int> choose(std::string_view, const std::vector<std::string>&, std::vector<float>*) { return detail::notBuilt(); }
  Result<TextScore> scoreText(std::string_view, std::string_view) { return detail::notBuilt(); }
  Result<std::string> simplify(std::string_view, std::string_view) { return detail::notBuilt(); }
};

inline Advisors makeAdvisors(Model&, const Config&) { return Advisors{}; }

#endif

}  // namespace vp::llm
