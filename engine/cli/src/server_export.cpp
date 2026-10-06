// Export, word/lemma lookups, model and online status.
#include <algorithm>
#include <cmath>
#include <filesystem>

#include "server.h"
#include "vp/fs.h"
#include "vp/online.h"
#include "vp/text.h"

namespace vpcli {

namespace fs = vp::fs;
using vp::ErrorCode;

ExportOptions Server::exportOptions(const json& p) const {
  const json s = settings_.get();
  const json ex = s.contains("export") && s["export"].is_object() ? s["export"] : json::object();
  ExportOptions o;
  o.format = format_;
  const std::string f = strParam(p, "format", "");
  if (!f.empty() && !formatFromName(f, o.format))
    fail(ErrorCode::UnsupportedFormat, "unknown export format '" + f + "'", "Choose SRT, VTT, ASS or TXT.");
  o.emoji = boolParam(p, "emoji", ex.value("emoji", false));
  o.macrons = boolParam(p, "macrons", ex.value("macrons", false));
  o.rebreak = boolParam(p, "rebreak", ex.value("rebreak", true));
  const std::string greek = strParam(p, "greek", "polytonic");
  if (greek != "polytonic" && greek != "monotonic")
    fail(ErrorCode::BadParams, "greek must be polytonic or monotonic", "The request was malformed.");
  o.monotonic = greek == "monotonic" && langs_.target == vp::rules::Lang::Grc;
  o.encoding = strParam(p, "encoding", "");
  if (const json* b = param(p, "bom")) {
    if (!b->is_boolean()) fail(ErrorCode::BadParams, "bom must be true or false", "The request was malformed.");
    o.bom = b->get<bool>();
  }
  o.cpsLimit = cpsLimit_;
  return o;
}

json Server::cmdExportWrite(const json& p) {
  requireProject();
  const std::string path = strParam(p, "path", "", true);
  if (path.empty()) fail(ErrorCode::BadParams, "empty path", "Choose where to save the file.");
  const bool overwrite = boolParam(p, "overwrite", false);
  const ExportOptions o = exportOptions(p);
  if (!overwrite && fs::fileExists(path)) {
    vp::Error e{ErrorCode::Io, "file exists: " + path, "The file already exists. Choose another name or allow overwriting."};
    throw CmdError{e};
  }
  std::vector<std::string> targets(project_.cues.size());
  for (size_t i = 0; i < project_.cues.size(); ++i) targets[i] = project_.cues[i].target;
  ExportResult r = unwrap(buildExport(doc_, targets, sources_, o));
  unwrap(fs::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(r.bytes.data()), r.bytes.size())));
  json warns = json::array();
  for (const ExportWarning& w : r.warnings)
    warns.push_back(json{{"index", w.index >= 0 ? json(w.index) : json(nullptr)}, {"kind", w.kind}});
  return json{{"path", path}, {"bytes", r.bytes.size()}, {"warnings", warns}};
}

json Server::cmdExportPreview(const json& p) {
  requireProject();
  const ExportOptions o = exportOptions(p);
  const json* idx = param(p, "indices");
  if (!idx || !idx->is_array()) fail(ErrorCode::BadParams, "missing array 'indices'", "The request was incomplete.");
  if (idx->size() > 200) fail(ErrorCode::BadParams, "at most 200 indices", "Preview fewer cues at once.");
  json cues = json::array();
  for (const json& v : *idx) {
    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() >= static_cast<int64_t>(project_.cues.size()))
      fail(ErrorCode::BadParams, "bad cue index in 'indices'", "A cue number was out of range.");
    const size_t pos = static_cast<size_t>(v.get<int64_t>());
    cues.push_back(json{{"index", pos},
                        {"lines", previewLines(doc_.cues[pos], project_.cues[pos].target, sources_[pos], o)}});
  }
  return json{{"cues", cues}};
}

// ---------------------------------------------------------------------------------------------- words
json Server::cmdWordInspect(const json& p) {
  const std::string text = strParam(p, "text", "", true);
  if (text.empty() || text.size() > 256) fail(ErrorCode::BadParams, "text must be 1..256 bytes", "Select one word.");
  const std::string code = strParam(p, "lang", "la");
  vp::rules::Lang lang;
  if (!langFromCode(code, lang)) fail(ErrorCode::BadParams, "unknown language '" + code + "'", "The request was malformed.");
  vp::rules::Options opt;
  if (hasProject_) opt = engineOptions();
  vp::rules::InspectResult r = unwrap(engine_->inspect(text, lang, opt));
  const vp::lex::Lexicon* lx = lexFor(lang);
  json analyses = json::array();
  for (const vp::rules::Analysis& a : r.analyses) {
    json lemma = lx ? lemmaJson(*lx, a.lemmaId) : json(nullptr);
    if (lemma.is_null())
      lemma = json{{"id", a.lemmaId}, {"head", a.head}, {"glossEn", a.glossEn}, {"glossEs", a.glossEs}, {"tier", a.tier}};
    analyses.push_back(json{{"lemma", lemma}, {"features", featureJson(a.features)}, {"display", a.display}});
  }
  return json{{"analyses", analyses}, {"suggestions", r.suggestions}};
}

json Server::cmdLemmaGet(const json& p) {
  const std::string code = strParam(p, "lang", "la");
  vp::rules::Lang lang;
  if (!langFromCode(code, lang)) fail(ErrorCode::BadParams, "unknown language '" + code + "'", "The request was malformed.");
  const vp::lex::Lexicon* lx = lexFor(lang);
  if (!lx) fail(ErrorCode::LexiconMissing, "no lexicon for '" + code + "'", "The dictionary for this language is not installed.");
  const int64_t id = intParam(p, "id", 0, 0, 0xFFFFFFFEll, true);
  json lemma = lemmaJson(*lx, static_cast<uint32_t>(id));
  if (lemma.is_null()) fail(ErrorCode::NotFound, "no lemma " + std::to_string(id), "That word is not in the dictionary.");
  std::vector<vp::lex::Sense> senses;
  lx->senses(static_cast<uint32_t>(id), senses);
  json sj = json::array();
  for (const vp::lex::Sense& s : senses) sj.push_back(senseJson(s));
  std::vector<std::pair<uint32_t, std::string_view>> cells;
  lx->cells(static_cast<uint32_t>(id), cells);
  json cj = json::array();
  for (const auto& c : cells) {
    json cell{{"features", featureJson(featuresFromPacked(c.first))}, {"form", std::string(c.second)}};
    const std::vector<std::string> extra = extraNames(vp::feat::unpack(c.first).extra);
    if (!extra.empty()) cell["extra"] = extra;
    cj.push_back(std::move(cell));
  }
  return json{{"lemma", lemma}, {"senses", sj}, {"cells", cj}};
}

// ---------------------------------------------------------------------------------------------- model, online
// Engine ii (engine/llm). The model file is settings.modelPath, else $VP_MODEL_GGUF, else the first *.gguf (by
// name) in <exe dir>/models or <data>/models. Nothing is ever downloaded here. The model is loaded only inside a
// translate job (lazily, on the engine's first question) or model.test, unloaded after it and after 60 s idle.
namespace {
std::string firstGguf(const std::string& dir) {
  std::string best;
  try {
    if (dir.empty() || !fs::isDirectory(dir)) return best;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = fs::toU8(it->path().filename());
      if (name.size() > 5 && name.compare(name.size() - 5, 5, ".gguf") == 0 && fs::isRegularFile(fs::join(dir, name)) &&
          (best.empty() || name < best))
        best = name;
    }
  } catch (...) {
    return std::string();
  }
  return best.empty() ? best : fs::join(dir, best);
}

const char* modelReason(vp::ErrorCode c) {
  switch (c) {
    case ErrorCode::ModelMissing: return "no_file";
    case ErrorCode::ModelUnsupportedCpu: return "cpu";
    default: return "bad_file";
  }
}
}  // namespace

std::string Server::modelPath() const {
  const std::string configured = settings_.get().value("modelPath", "");
  if (!configured.empty()) return configured;
  const std::string env = envU8("VP_MODEL_GGUF");
  if (!env.empty()) return env;
  std::string p = exeDir().empty() ? std::string() : firstGguf(fs::join(exeDir(), "models"));
  if (p.empty()) p = firstGguf(fs::join(opt_.dataDir, "models"));
  return p;
}

vp::llm::Config Server::modelConfig() const {
  vp::llm::Config c;
  c.path = modelPath();
  c.threads = settings_.get().value("eco", false) ? 2 : vp::llm::kMaxThreads;
  return c;
}

// `hash`: verify the SHA-256 (about 1-2 s for the first call per file, cached after). engine.hello passes false
// and reports a file of the right size as available until a hash says otherwise.
json Server::modelStatusJson(bool hash) {
  const std::string path = modelPath();
  json j{{"available", false}, {"path", path}, {"sizeBytes", 0}, {"sha256ok", false}, {"loaded", model_.loaded()},
         {"cpuOk", vp::llm::cpuSupported()}, {"lastLoadMs", model_.status().lastLoadMs},
         {"rerankEnabled", vp::llm::rerankEnabled()}};
  if (!vp::llm::built()) {
    j["reason"] = "not_built";
    return j;
  }
  if (path.empty()) {
    j["reason"] = "no_file";
    return j;
  }
  vp::Result<vp::llm::Status> st = model_.inspect(path, hash);
  if (!st) {
    j["reason"] = modelReason(st.error().code);
    return j;
  }
  const vp::llm::Status& s = st.value();
  j["sizeBytes"] = s.sizeBytes;
  j["sha256ok"] = s.sha256ok;
  const bool sizeOk = s.sizeBytes == vp::llm::kModelSizeBytes;
  j["available"] = hash ? s.available : (s.sha256ok || sizeOk);
  if (!j["available"].get<bool>()) j["reason"] = "sha_mismatch";
  else if (!s.cpuOk) j["reason"] = "cpu";
  return j;
}

bool Server::modelUsable(std::string& why) {
  if (!vp::llm::built()) {
    why = "model_missing";
    return false;
  }
  if (!vp::llm::cpuSupported()) {
    why = "model_unsupported_cpu";
    return false;
  }
  const json st = modelStatusJson(true);
  if (!st.value("available", false)) {
    why = st.value("reason", "") == "no_file" ? "model_missing" : "model_load_failed";
    return false;
  }
  return true;
}

json Server::cmdModelStatus(const json&) { return modelStatusJson(true); }

json Server::cmdModelUnload(const json&) {
  model_.unload();
  return modelStatusJson(false);
}

json Server::cmdModelLocate(const json& p) {
  const std::string path = strParam(p, "path", "", true);
  if (!fs::isRegularFile(path)) fail(ErrorCode::NotFound, "no file at " + path, "The model file was not found.");
  if (!vp::llm::built())
    fail(ErrorCode::ModelMissing, "the local model engine is not part of this build",
         "The local model is not available in this version.");
  vp::Result<vp::llm::FileCheck> fc = vp::llm::checkModelFile(path, true);
  if (!fc) throw CmdError{fc.error()};
  unwrap(settings_.set(json{{"modelPath", path}}));
  refreshSettingsCache();
  if (model_.loaded()) model_.unload();
  return modelStatusJson(true);
}

json Server::cmdModelTest(const json&) {
  if (!vp::llm::built())
    fail(ErrorCode::ModelMissing, "the local model engine is not part of this build",
         "The local model is not available in this version.");
  if (!vp::llm::cpuSupported())
    fail(ErrorCode::ModelUnsupportedCpu, "CPU lacks AVX2/FMA/F16C/BMI2",
         "This computer's processor lacks the AVX2 instructions the local model needs.");
  const json st = modelStatusJson(true);
  if (!st.value("available", false)) {
    if (st.value("reason", "") == "no_file")
      fail(ErrorCode::ModelMissing, "no model file", "The local model is not installed. Choose its file in Settings.");
    fail(ErrorCode::ModelLoadFailed, "model file check failed: " + st.value("reason", std::string()),
         "The local model file is not the expected one. Reinstall the local model.");
  }
  struct UnloadAfter {
    vp::llm::Model& m;
    ~UnloadAfter() { m.unload(); }
  } guard{model_};
  const int64_t t0 = monoMs();
  unwrap(model_.load(modelConfig()));
  const int64_t t1 = monoMs();
  const std::vector<std::string> options = {"puella", "puer", "aqua"};
  std::vector<float> scores;
  const int chosen = unwrap(model_.choose("Which word means 'girl'?", options, &scores));
  const int64_t t2 = monoMs();
  json sj = json::array();
  for (float f : scores) sj.push_back(std::round(static_cast<double>(f) * 1000.0) / 1000.0);
  model_.unload();
  json out = modelStatusJson(false);
  out["test"] = json{{"chosen", chosen}, {"chosenText", options[static_cast<size_t>(chosen)]}, {"correct", chosen == 0},
                     {"scores", sj}, {"loadMs", t1 - t0}, {"chooseMs", t2 - t1}, {"ms", t2 - t0}};
  return out;
}

// Engine iii (engine/online). Runs on the worker thread: the client may sleep for its 1 request/s throttle or a
// short Retry-After while the reader thread keeps answering engine.ping. The transport is created only when the
// settings allow it (engines.online && online.wiktionary); otherwise nothing network-related is even constructed.
json Server::cmdOnlineTest(const json&) {
  if (!vp::online::onlineAllowed(settings_))
    fail(ErrorCode::OnlineDisabled, "online check is turned off", "Turn on the online check in Settings first.");
  const char* const kNoNet = "Could not reach wiktionary.org. Check the internet connection and try again.";
  vp::Result<std::unique_ptr<vp::online::Transport>> transport = vp::online::makeSystemTransport(settings_);
  if (!transport) fail(transport.error().code, transport.error().message, transport.error().hint.empty() ? kNoNet : transport.error().hint);
  vp::online::Config cfg;
  cfg.cacheDir = fs::join(opt_.dataDir, "online-cache");
  cfg.maxRetryAfterSec = 5;   // a test answers quickly: a longer Retry-After reports "rate limited"
  vp::Result<std::unique_ptr<vp::online::Wiktionary>> client =
      vp::online::Wiktionary::create(*transport.value(), settings_, cfg);
  if (!client) fail(client.error().code, client.error().message, client.error().hint);
  const int64_t t0 = monoMs();
  const vp::Result<vp::online::Evidence> ev = client.value()->probe();   // "aqua", cache not read
  const int64_t ms = monoMs() - t0;
  if (!ev) fail(ev.error().code, ev.error().message, ev.error().hint);
  using Ev = vp::online::Evidence;
  if (ev->verdict == Ev::Error) {
    if (ev->summary == "online disabled")
      fail(ErrorCode::OnlineDisabled, "online check is turned off", "Turn on the online check in Settings first.");
    fail(ErrorCode::OnlineFailed, ev->summary, kNoNet);
  }
  const bool ok = ev->verdict == Ev::Agrees || ev->verdict == Ev::Disagrees;
  const std::string message = ok ? "wiktionary.org answered in " + std::to_string(ms) + " ms"
                                 : "wiktionary.org: " + ev->summary;
  return json{{"ok", ok}, {"latencyMs", ms}, {"message", message}, {"verdict", vp::online::verdictName(ev->verdict)},
              {"summary", ev->summary}, {"url", ev->url}};
}

}  // namespace vpcli
