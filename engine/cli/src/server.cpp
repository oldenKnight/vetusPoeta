// `vpengine serve` core: framing, reader-side commands, the worker loop, autosave, settings and project commands.
#include "server.h"

#include <algorithm>
#include <exception>
#include <unordered_map>

#include "vp/fs.h"
#include "vp/text.h"

namespace vpcli {

namespace fs = vp::fs;
using vp::ErrorCode;

// ---------------------------------------------------------------------------------------------- helpers
void fail(ErrorCode code, std::string message, std::string hint) {
  throw CmdError{vp::Error{code, std::move(message), std::move(hint)}};
}

const json* param(const json& p, const char* key) {
  if (!p.is_object()) return nullptr;
  auto it = p.find(key);
  if (it == p.end() || it->is_null()) return nullptr;
  return &*it;
}

int64_t intParam(const json& p, const char* key, int64_t def, int64_t lo, int64_t hi, bool required) {
  const json* v = param(p, key);
  if (!v) {
    if (required) fail(ErrorCode::BadParams, std::string("missing parameter '") + key + "'", "The request was incomplete.");
    return def;
  }
  int64_t x = 0;
  if (v->is_number_integer()) {
    x = v->get<int64_t>();
  } else if (v->is_number_float()) {
    const double d = v->get<double>();
    if (!(d == static_cast<double>(static_cast<int64_t>(d))))
      fail(ErrorCode::BadParams, std::string("parameter '") + key + "' must be an integer", "The request was malformed.");
    x = static_cast<int64_t>(d);
  } else {
    fail(ErrorCode::BadParams, std::string("parameter '") + key + "' must be an integer", "The request was malformed.");
  }
  if (x < lo || x > hi)
    fail(ErrorCode::BadParams, std::string("parameter '") + key + "' out of range", "A value was out of range.");
  return x;
}

std::string strParam(const json& p, const char* key, const std::string& def, bool required) {
  const json* v = param(p, key);
  if (!v) {
    if (required) fail(ErrorCode::BadParams, std::string("missing parameter '") + key + "'", "The request was incomplete.");
    return def;
  }
  if (!v->is_string())
    fail(ErrorCode::BadParams, std::string("parameter '") + key + "' must be a string", "The request was malformed.");
  return v->get<std::string>();
}

bool boolParam(const json& p, const char* key, bool def) {
  const json* v = param(p, key);
  if (!v) return def;
  if (!v->is_boolean())
    fail(ErrorCode::BadParams, std::string("parameter '") + key + "' must be true or false", "The request was malformed.");
  return v->get<bool>();
}

// ---------------------------------------------------------------------------------------------- queue
bool RequestQueue::tryPush(json req) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_ || q_.size() >= kCap) return false;
    q_.push_back(std::move(req));
  }
  cv_.notify_one();
  return true;
}

bool RequestQueue::pop(json& req, int timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return closed_ || !q_.empty(); });
  if (q_.empty()) return false;
  req = std::move(q_.front());
  q_.pop_front();
  return true;
}

void RequestQueue::close() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
  }
  cv_.notify_all();
}

// ---------------------------------------------------------------------------------------------- server
namespace {
const char* const kLexFiles[4] = {"latin.vpl", "greek.vpl", "english.vpl", "spanish.vpl"};
const char* const kLexLangs[4] = {"la", "grc", "en", "es"};
const char* const kInternalHint = "Something went wrong inside the engine. Your project is safe; please try again.";

int64_t envInt(const char* name, int64_t def, int64_t lo, int64_t hi) {
  const std::string v = envU8(name);
  if (v.empty()) return def;
  try {
    return std::max(lo, std::min(hi, static_cast<int64_t>(std::stoll(v))));
  } catch (...) {
    return def;
  }
}
}  // namespace

Server::Server(Output& out, ServeOptions opt)
    : out_(out), opt_(std::move(opt)), settings_(fs::join(opt_.dataDir, "settings.json")) {
  settings_.load();
  for (const std::string& w : settings_.warnings()) logMsg(LogLevel::Warn, "settings: " + w);
  vp::llm::setLogSink([](int level, const char* text) {
    logMsg(level == 1 ? LogLevel::Error : LogLevel::Warn, std::string("llm: ") + text);
  });
  refreshSettingsCache();
  autosaveDebounceMs_ = envInt("VP_AUTOSAVE_MS", 5000, 50, 600000);
  autosaver_ = vp::Autosaver(autosaveDebounceMs_, std::max<int64_t>(60000, autosaveDebounceMs_));

  for (int i = 0; i < 4; ++i) {
    const std::string path = fs::join(opt_.lexiconDir, kLexFiles[i]);
    json info{{"lang", kLexLangs[i]}, {"path", path}, {"available", false}};
    vp::Result<vp::lex::Lexicon> r = vp::lex::Lexicon::open(fs::u8path(path));
    if (r) {
      lex_[i] = std::move(r.value());
      lexOk_[i] = true;
      const vp::lex::Stats st = lex_[i].stats();
      info["available"] = true;
      info["version"] = std::to_string(st.major) + "." + std::to_string(st.minor);
      info["lemmas"] = lex_[i].lemmaCount();
      info["notice"] = std::string(lex_[i].notice());
      if (lex_[i].lang() != kLexLangs[i]) info["langInFile"] = std::string(lex_[i].lang());
      logMsg(LogLevel::Info, std::string("lexicon ") + kLexLangs[i] + ": " + path);
    } else {
      info["error"] = {{"code", vp::errorCodeName(r.error().code)}, {"message", r.error().message}};
      logMsg(r.error().code == ErrorCode::LexiconMissing ? LogLevel::Debug : LogLevel::Warn,
             std::string("lexicon ") + kLexLangs[i] + ": " + r.error().message);
    }
    lexInfo_.push_back(std::move(info));
  }
#if defined(VP_HAVE_RULES)
  engine_ = vp::rules::makeEngine();
#else
  engine_ = vp::rules::makeStubEngine();
#endif
  vp::Result<void> lr = engine_->setLexicons(lexOk_[0] ? &lex_[0] : nullptr, lexOk_[1] ? &lex_[1] : nullptr,
                                             lexOk_[2] ? &lex_[2] : nullptr, lexOk_[3] ? &lex_[3] : nullptr);
  if (!lr) logMsg(LogLevel::Warn, "engine lexicons: " + lr.error().message);
}

Server::~Server() = default;

void Server::sendOk(const json& id, json result) {
  out_.send(json{{"id", id}, {"ok", true}, {"result", std::move(result)}});
}

void Server::sendError(const json& id, const vp::Error& e) {
  json err{{"code", vp::errorCodeName(e.code)}, {"message", e.message},
           {"hint", e.hint.empty() ? std::string(kInternalHint) : e.hint}};
  out_.send(json{{"id", id}, {"ok", false}, {"error", std::move(err)}});
}

void Server::sendEvent(const char* name, json body) {
  body["event"] = name;
  out_.send(body);
}

void Server::requestStop() {
  stop_.store(true);
  queue_.close();
}

// ---- reader thread ----
void Server::onLine(std::string&& line) {
  json id = nullptr;
  try {
    json req = json::parse(line, nullptr, false);
    if (req.is_discarded()) {
      sendError(nullptr, vp::Error{ErrorCode::BadParams, "malformed JSON line", "The engine received a damaged request."});
      return;
    }
    if (!req.is_object()) {
      sendError(nullptr, vp::Error{ErrorCode::BadParams, "a request must be a JSON object",
                                   "The engine received a damaged request."});
      return;
    }
    auto iid = req.find("id");
    if (iid != req.end() && (iid->is_number_integer() || iid->is_string())) id = *iid;
    auto ic = req.find("cmd");
    if (id.is_null() || ic == req.end() || !ic->is_string()) {
      sendError(id, vp::Error{ErrorCode::BadParams, "a request needs an integer 'id' and a string 'cmd'",
                              "The engine received a damaged request."});
      return;
    }
    auto ip = req.find("params");
    if (ip != req.end() && !ip->is_null() && !ip->is_object()) {
      sendError(id, vp::Error{ErrorCode::BadParams, "'params' must be an object", "The engine received a damaged request."});
      return;
    }
    if (ip == req.end() || ip->is_null()) req["params"] = json::object();
    const std::string& cmd = ic->get_ref<const std::string&>();
    if (cmd == "engine.ping") {
      sendOk(id, json::object());
      return;
    }
    if (cmd == "engine.shutdown") {
      sendOk(id, json::object());
      logMsg(LogLevel::Info, "shutdown requested");
      cancelJob_.store(runningJob_.load());
      requestStop();
      return;
    }
    if (cmd.size() > 7 && cmd.compare(cmd.size() - 7, 7, ".cancel") == 0) {
      const json& p = req["params"];
      int64_t job = runningJob_.load();
      auto ij = p.find("jobId");
      if (ij != p.end() && ij->is_number_integer()) job = ij->get<int64_t>();
      if (job > 0) cancelJob_.store(job);
      sendOk(id, json::object());
      return;
    }
    if (stop_.load()) {
      sendError(id, vp::Error{ErrorCode::Busy, "the engine is shutting down", "The engine is closing."});
      return;
    }
    if (!queue_.tryPush(std::move(req)))
      sendError(id, vp::Error{ErrorCode::Busy, "request queue full", "The engine is busy. Try again in a moment."});
  } catch (const std::exception& e) {
    sendError(id, vp::Error{ErrorCode::Internal, std::string("reader: ") + e.what(), kInternalHint});
  } catch (...) {
    sendError(id, vp::Error{ErrorCode::Internal, "reader: unknown error", kInternalHint});
  }
}

void Server::onLineTooLong() {
  sendError(nullptr, vp::Error{ErrorCode::BadParams, "request line too long", "The request was too large."});
}

void Server::onEof() {
  logMsg(LogLevel::Info, "stdin closed");
  cancelJob_.store(runningJob_.load());
  requestStop();
}

// ---- worker ----
int Server::run() {
  logMsg(LogLevel::Info, std::string("serving (data ") + opt_.dataDir + ", lexicons " + opt_.lexiconDir + ")");
  lastTickMs_ = monoMs();
  // engine.shutdown and EOF close the queue: requests already queued still run (jobs see stop_ and end at once),
  // then the loop ends. A termination signal ends it at the next pop.
  while (!stopRequested()) {
    json req;
    if (queue_.pop(req, 200)) handle(req);
    else if (stop_.load()) break;
    tick(false);
    if (out_.broken()) {
      logMsg(LogLevel::Warn, "stdout closed");
      break;
    }
  }
  if (stopRequested()) logMsg(LogLevel::Info, "stop signal");
  requestStop();
  finish();
  return 0;
}

void Server::finish() {
  try {
    if (hasProject_) {
      if (autosaver_.dirty() && settings_.get().value("autosave", true)) writeAutosave();
      if (ownLock_) vp::removeLock(lockBase_);
      ownLock_ = false;
    }
  } catch (...) {
    logMsg(LogLevel::Error, "cleanup failed");
  }
}

void Server::handle(const json& req) {
  static const std::unordered_map<std::string, Handler> kHandlers = {
      {"engine.hello", &Server::cmdHello},
      {"settings.get", &Server::cmdSettingsGet},
      {"settings.set", &Server::cmdSettingsSet},
      {"project.new", &Server::cmdProjectNew},
      {"project.open", &Server::cmdProjectOpen},
      {"project.recover", &Server::cmdProjectRecover},
      {"project.save", &Server::cmdProjectSave},
      {"project.saveAs", &Server::cmdProjectSaveAs},
      {"project.close", &Server::cmdProjectClose},
      {"cue.page", &Server::cmdCuePage},
      {"cue.get", &Server::cmdCueGet},
      {"cue.set", &Server::cmdCueSet},
      {"cue.choose", &Server::cmdCueChoose},
      {"cue.review", &Server::cmdCueReview},
      {"translate.start", &Server::cmdTranslateStart},
      {"orbergise.start", &Server::cmdOrbergiseStart},
      {"word.inspect", &Server::cmdWordInspect},
      {"lemma.get", &Server::cmdLemmaGet},
      {"names.list", &Server::cmdNamesList},
      {"names.set", &Server::cmdNamesSet},
      {"corrections.list", &Server::cmdCorrectionsList},
      {"corrections.remove", &Server::cmdCorrectionsRemove},
      {"words.list", &Server::cmdWordsList},
      {"export.write", &Server::cmdExportWrite},
      {"export.preview", &Server::cmdExportPreview},
      {"model.status", &Server::cmdModelStatus},
      {"model.unload", &Server::cmdModelUnload},
      {"model.locate", &Server::cmdModelLocate},
      {"model.test", &Server::cmdModelTest},
      {"online.test", &Server::cmdOnlineTest},
      {"history.undo", &Server::cmdHistoryUndo},
      {"history.redo", &Server::cmdHistoryRedo},
      {"eval.run", &Server::cmdEvalRun},
  };
  const json id = req.value("id", json(nullptr));
  std::string cmd;
  try {
    cmd = req.at("cmd").get<std::string>();
    auto it = kHandlers.find(cmd);
    if (it == kHandlers.end())
      fail(ErrorCode::BadParams, "unknown command '" + cmd + "'", "This engine version does not know that command.");
    const int64_t t0 = monoMs();
    json result = (this->*(it->second))(req.at("params"));
    sendOk(id, std::move(result));
    if (logEnabled(LogLevel::Debug)) logMsg(LogLevel::Debug, cmd + " ok " + std::to_string(monoMs() - t0) + " ms");
  } catch (const CmdError& e) {
    logMsg(LogLevel::Debug, cmd + ": " + vp::errorCodeName(e.error.code) + " " + e.error.message);
    sendError(id, e.error);
  } catch (const std::exception& e) {
    logMsg(LogLevel::Error, cmd + ": " + e.what());
    sendError(id, vp::Error{ErrorCode::Internal, cmd + ": " + e.what(), kInternalHint});
  } catch (...) {
    logMsg(LogLevel::Error, cmd + ": unknown exception");
    sendError(id, vp::Error{ErrorCode::Internal, cmd + ": unknown error", kInternalHint});
  }
  if (pendingJob_) {
    std::function<void()> job;
    job.swap(pendingJob_);
    try {
      job();
    } catch (const std::exception& e) {
      logMsg(LogLevel::Error, std::string("job: ") + e.what());
      runningJob_.store(0);
    } catch (...) {
      logMsg(LogLevel::Error, "job: unknown exception");
      runningJob_.store(0);
    }
  }
}

void Server::tick(bool force) {
  const int64_t now = monoMs();
  if (!force && now - lastTickMs_ < kTickMs) return;
  lastTickMs_ = now;
  if (model_.maybeUnload(now)) logMsg(LogLevel::Info, "local model unloaded after idle time");
  if (!hasProject_ || !autosaver_.due(now)) return;
  if (!settings_.get().value("autosave", true)) return;
  if (writeAutosave()) {
    autosaver_.saved(now);
    sendEvent("project.autosaved", json{{"path", vp::autosavePath(lockBase_)}, {"at", lastAutosaveAt_}});
  } else {
    autosaver_.saveFailed(now);
  }
}

bool Server::writeAutosave() {
  try {
    project_.stats = statsJson();
    vp::Result<void> r = project_.save(vp::autosavePath(lockBase_));
    if (!r) {
      logMsg(LogLevel::Warn, "autosave failed: " + r.error().message);
      return false;
    }
    lastAutosaveAt_ = fs::isoUtc(fs::nowUnixMs());
    logMsg(LogLevel::Debug, "autosaved " + vp::autosavePath(lockBase_));
    return true;
  } catch (const std::exception& e) {
    logMsg(LogLevel::Warn, std::string("autosave failed: ") + e.what());
    return false;
  }
}

void Server::noteChange() { autosaver_.noteChange(monoMs()); }

// ---------------------------------------------------------------------------------------------- engine, settings
json Server::cmdHello(const json&) {
  const json s = settings_.get();
  return json{{"version", vp::appVersion()},
              {"engine", engine_->version()},
              {"lexicons", lexInfo_},
              {"model", modelStatusJson(false)},
              {"threads", s.value("eco", false) ? 2 : hardwareThreads()},
              {"dataDir", opt_.dataDir},
              {"lexiconDir", opt_.lexiconDir},
              {"settingsWarnings", settings_.warnings()}};
}

json Server::cmdSettingsGet(const json&) { return settings_.get(); }

json Server::cmdSettingsSet(const json& p) {
  const json* patch = param(p, "patch");
  if (!patch || !patch->is_object()) fail(ErrorCode::BadParams, "missing object 'patch'", "The request was incomplete.");
  json out = unwrap(settings_.set(*patch));
  refreshSettingsCache();
  return out;
}

void Server::refreshSettingsCache() {
  const json s = settings_.get();
  cpsLimit_ = 17;
  if (s.contains("cps") && s["cps"].is_object() && s["cps"].contains("adult") && s["cps"]["adult"].is_number())
    cpsLimit_ = s["cps"]["adult"].get<double>();
}

void Server::addRecent(const std::string& path) {
  try {
    json s = settings_.get();
    json list = json::array({path});
    if (s.contains("recentProjects") && s["recentProjects"].is_array())
      for (const json& e : s["recentProjects"])
        if (e.is_string() && e.get<std::string>() != path && list.size() < vp::Settings::kMaxRecentProjects)
          list.push_back(e);
    vp::Result<json> r = settings_.set(json{{"recentProjects", list}});
    if (!r) logMsg(LogLevel::Warn, "recent projects: " + r.error().message);
  } catch (...) {
  }
}

const vp::lex::Lexicon* Server::lexFor(vp::rules::Lang l) const {
  const int i = l == vp::rules::Lang::La ? 0 : l == vp::rules::Lang::Grc ? 1 : l == vp::rules::Lang::En ? 2 : 3;
  return lexOk_[i] ? &lex_[i] : nullptr;
}

// ---------------------------------------------------------------------------------------------- project
void Server::requireProject() const {
  if (!hasProject_) fail(ErrorCode::BadParams, "no project is open", "Open or create a project first.");
}

size_t Server::position(const json& p, const char* key) const {
  requireProject();
  const int64_t idx = intParam(p, key, 0, 0, static_cast<int64_t>(project_.cues.size()) - 1, true);
  return static_cast<size_t>(idx);
}

void Server::loadDocument(std::vector<std::string>& warnings) {
  vp::subs::Format f = vp::subs::Format::Txt;
  if (project_.manifest.kind != "text" && !formatFromPath(project_.manifest.sourceFileName, f))
    fail(ErrorCode::UnsupportedFormat, "unknown subtitle type: " + project_.manifest.sourceFileName,
         "Only .srt, .vtt, .ass, .ssa and .txt files can be opened.");
  std::vector<uint8_t> bytes(project_.source.begin(), project_.source.end());
  vp::subs::Document d = unwrap(vp::subs::parse(bytes, f));
  format_ = d.format;
  sources_.clear();
  sources_.reserve(d.cues.size());
  for (const vp::subs::Cue& c : d.cues) sources_.push_back(c.plainText());
  if (!project_.cues.empty() && project_.cues.size() != d.cues.size())
    warnings.push_back("the project had " + std::to_string(project_.cues.size()) + " cues for " +
                       std::to_string(d.cues.size()) + " in the source; the list was adjusted");
  project_.cues.resize(d.cues.size());
  for (size_t i = 0; i < d.cues.size(); ++i) project_.cues[i].index = static_cast<int>(i);   // protocol index = 0-based position
  doc_ = std::move(d);
  if (!parsePair(project_.manifest.pair, langs_)) langs_ = PairLangs{};
}

void Server::installProject(vp::Project&& p, const std::string& path, const std::string& lockBase, bool takeLock,
                            std::vector<std::string>& warnings) {
  project_ = std::move(p);
  loadDocument(warnings);   // may throw: the previous project is already closed
  hasProject_ = true;
  path_ = path;
  lockBase_ = lockBase;
  ownLock_ = false;
  if (takeLock) {
    vp::Result<void> r = vp::writeLock(lockBase_);
    if (r) ownLock_ = true;
    else warnings.push_back("the lock file could not be written: " + r.error().message);
  }
  undo_.clear();
  redo_.clear();
  historyRecords_ = 0;
  autosaver_ = vp::Autosaver(autosaveDebounceMs_, std::max<int64_t>(60000, autosaveDebounceMs_));
  nextCorrection_ = 1;
  for (const vp::Correction& c : project_.corrections)
    if (c.id.size() > 1 && c.id[0] == 'c') {
      try {
        nextCorrection_ = std::max(nextCorrection_, std::stoi(c.id.substr(1)) + 1);
      } catch (...) {
      }
    }
}

void Server::closeProject(bool discardAutosave) {
  if (!hasProject_) return;
  if (discardAutosave) fs::removeQuiet(vp::autosavePath(lockBase_));
  if (ownLock_) vp::removeLock(lockBase_);
  hasProject_ = false;
  ownLock_ = false;
  project_ = vp::Project();
  doc_ = vp::subs::Document();
  sources_.clear();
  sources_.shrink_to_fit();
  path_.clear();
  lockBase_.clear();
  undo_.clear();
  redo_.clear();
  historyRecords_ = 0;
  autosaver_ = vp::Autosaver(autosaveDebounceMs_, std::max<int64_t>(60000, autosaveDebounceMs_));
}

json Server::statsJson() const {
  int n[5] = {0, 0, 0, 0, 0};   // new translated edited reviewed stale
  int c[3] = {0, 0, 0};
  for (const vp::CueRecord& r : project_.cues) {
    if (r.state == "translated") ++n[1];
    else if (r.state == "edited") ++n[2];
    else if (r.state == "reviewed") ++n[3];
    else if (r.state == "stale") ++n[4];
    else ++n[0];
    if (r.confidence == "ok") ++c[0];
    else if (r.confidence == "check") ++c[1];
    else if (r.confidence == "fix") ++c[2];
  }
  return json{{"total", project_.cues.size()}, {"new", n[0]}, {"translated", n[1]}, {"edited", n[2]},
              {"reviewed", n[3]}, {"stale", n[4]}, {"ok", c[0]}, {"check", c[1]}, {"fix", c[2]}};
}

json Server::projectJson() const {
  const vp::Manifest& m = project_.manifest;
  json warns = json::array();
  for (const vp::subs::Warning& w : doc_.warnings) {
    if (warns.size() >= 200) break;
    warns.push_back(json{{"index", w.index ? json(w.index - 1) : json(nullptr)}, {"kind", w.kind}});
  }
  return json{{"path", path_.empty() ? json(nullptr) : json(path_)},
              {"autosavePath", vp::autosavePath(lockBase_)},
              {"manifest", {{"format", m.format}, {"appVersion", m.appVersion}, {"created", m.created},
                            {"pair", m.pair}, {"kind", m.kind}, {"sourceFileName", m.sourceFileName},
                            {"sourceSha256", m.sourceSha256}}},
              {"stats", statsJson()},
              {"subs", {{"format", formatName(format_)}, {"encoding", doc_.encoding}, {"bom", doc_.bom},
                        {"newline", doc_.newline == "\r\n" ? "crlf" : "lf"}, {"warnings", warns}}},
              {"dirty", autosaver_.dirty()},
              {"canUndo", !undo_.empty()},
              {"canRedo", !redo_.empty()}};
}

json Server::cmdProjectNew(const json& p) {
  const std::string kind = strParam(p, "kind", "subs");
  if (kind != "subs" && kind != "text") fail(ErrorCode::BadParams, "kind must be subs or text", "Choose subtitles or text.");
  const std::string pair = strParam(p, "pair", settings_.get().value("defaultPair", "en-la"));
  PairLangs pl;
  if (!parsePair(pair, pl)) fail(ErrorCode::BadParams, "unknown language pair '" + pair + "'", "Choose a language pair.");
  const std::string sourcePath = strParam(p, "sourcePath", "");
  std::string bytes, name;
  if (!sourcePath.empty()) {
    vp::subs::Format f;
    if (!formatFromPath(sourcePath, f))
      fail(ErrorCode::UnsupportedFormat, "unknown file type: " + sourcePath,
           "Only .srt, .vtt, .ass, .ssa and .txt files can be opened.");
    if (kind == "subs" && f == vp::subs::Format::Txt)
      fail(ErrorCode::UnsupportedFormat, "a .txt file is not a subtitle file", "Open a .txt file as a text project.");
    if (kind == "text" && f != vp::subs::Format::Txt)
      fail(ErrorCode::UnsupportedFormat, "a text project needs a .txt file", "Open subtitle files as subtitles.");
    bytes = unwrap(fs::readFile(sourcePath, 256ull << 20));
    name = baseName(sourcePath);
  } else if (kind == "text") {
    bytes = strParam(p, "text", "", true);
    name = "text.txt";
  } else {
    fail(ErrorCode::BadParams, "missing parameter 'sourcePath'", "Choose a subtitle file.");
  }
  const vp::ProjectKind pk = kind == "text" ? vp::ProjectKind::Text : vp::ProjectKind::Subs;
  vp::Project proj = unwrap(vp::Project::create(pk, pair, std::move(bytes), name, settings_.get()));
  const std::string savePath = strParam(p, "path", "");
  std::string lockBase = savePath;
  if (lockBase.empty()) {
    const std::string dir = fs::join(opt_.dataDir, "unsaved");
    unwrap(fs::createDirectories(dir));
    lockBase = fs::join(dir, "untitled-" + std::to_string(fs::nowUnixMs()) + ".vpoeta");
  }
  closeProject(false);
  std::vector<std::string> warnings;
  installProject(std::move(proj), std::string(), lockBase, true, warnings);
  if (!savePath.empty()) {
    project_.stats = statsJson();
    vp::Result<void> r = project_.save(savePath);
    if (!r) {
      closeProject(true);
      throw CmdError{r.error()};
    }
    path_ = savePath;
    addRecent(savePath);
  } else {
    noteChange();   // an unsaved project is autosaved under <data>/unsaved
  }
  json out{{"project", projectJson()}, {"warnings", warnings}};
  return out;
}

json Server::cmdProjectOpen(const json& p) {
  const std::string path = strParam(p, "path", "", true);
  vp::Opened o = unwrap(vp::Project::open(path));
  const vp::Recovery rec = o.recoverable;
  closeProject(false);
  std::vector<std::string> warnings = o.warnings;
  installProject(std::move(o.project), path, path, !rec.lockedByLiveProcess, warnings);
  addRecent(path);
  json out{{"project", projectJson()}, {"warnings", warnings}};
  if (rec.available) out["recoverable"] = json{{"autosavePath", rec.autosavePath}, {"at", fs::isoUtc(rec.atMs)}};
  else out["recoverable"] = nullptr;
  if (rec.lockedByLiveProcess) out["lockedBy"] = rec.lockPid;
  return out;
}

json Server::cmdProjectRecover(const json& p) {
  const std::string path = strParam(p, "path", "", true);
  vp::Project proj = unwrap(vp::Project::recover(path));
  closeProject(false);
  std::vector<std::string> warnings;
  installProject(std::move(proj), path, path, true, warnings);
  noteChange();   // the recovered state is newer than the project file
  return json{{"path", path}, {"at", fs::isoUtc(fs::nowUnixMs())}, {"project", projectJson()}, {"warnings", warnings}};
}

json Server::cmdProjectSave(const json& p) {
  requireProject();
  const std::string target = strParam(p, "path", path_);
  if (target.empty()) fail(ErrorCode::BadParams, "the project has no file yet", "Choose where to save the project.");
  project_.stats = statsJson();
  unwrap(project_.save(target));
  if (target != lockBase_) {
    fs::removeQuiet(vp::autosavePath(lockBase_));
    if (ownLock_) vp::removeLock(lockBase_);
    lockBase_ = target;
    ownLock_ = static_cast<bool>(vp::writeLock(lockBase_));
  }
  path_ = target;
  fs::removeQuiet(vp::autosavePath(target));
  autosaver_.saved(monoMs());
  addRecent(target);
  return json{{"path", target}, {"at", fs::isoUtc(fs::nowUnixMs())}};
}

json Server::cmdProjectSaveAs(const json& p) {
  strParam(p, "path", "", true);
  return cmdProjectSave(p);
}

json Server::cmdProjectClose(const json& p) {
  const bool discard = boolParam(p, "discard", false);
  const std::string path = path_;
  closeProject(discard);
  return json{{"path", path.empty() ? json(nullptr) : json(path)}, {"at", fs::isoUtc(fs::nowUnixMs())}};
}

}  // namespace vpcli
