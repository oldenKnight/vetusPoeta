// `vpengine serve`: the JSON-lines engine process (DESIGN §9). Threads:
//   reader - reads stdin lines, parses them, answers engine.ping, engine.shutdown and *.cancel itself (so they are
//            answered at once even while a job runs) and queues everything else (bounded, 1,024; full -> busy);
//   worker - the thread that calls run(): executes queued commands one at a time against the single open project,
//            runs translate/orbergise jobs (cancelled at cue granularity) and ticks the autosave every second.
// Responses and events go through one Output (one JSON object per line). Every command catches at the boundary.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "serve_io.h"
#include "views.h"
#include "vp/lex.h"
#include "vp/llm.h"
#include "vp/online.h"
#include "vp/project.h"
#include "vp/result.h"
#include "vp/engine_config.h"
#include "vp/rules.h"
#include "vp/settings.h"
#include "vp/subs.h"

namespace vpcli {

// A command failure inside the server (never leaves the server: dispatch turns it into an error response).
struct CmdError {
  vp::Error error;
};
[[noreturn]] void fail(vp::ErrorCode code, std::string message, std::string hint);
template <class T> T unwrap(vp::Result<T>&& r) {
  if (!r) throw CmdError{r.error()};
  return std::move(r.value());
}
inline void unwrap(vp::Result<void>&& r) {
  if (!r) throw CmdError{r.error()};
}

// Parameter accessors: wrong types or ranges throw bad_params with a hint.
const json* param(const json& p, const char* key);   // nullptr when missing or null
int64_t intParam(const json& p, const char* key, int64_t def, int64_t lo, int64_t hi, bool required = false);
std::string strParam(const json& p, const char* key, const std::string& def, bool required = false);
bool boolParam(const json& p, const char* key, bool def);

struct ServeOptions {
  std::string dataDir;      // settings.json, unsaved autosaves, samples/, online-cache/
  std::string lexiconDir;   // latin.vpl, greek.vpl, english.vpl, spanish.vpl (+ nlp/, curated/ in the dist)
  bool stub = false;        // --stub or VP_FORCE_STUB=1: the echo engine of rules_stub.cpp (protocol tests)
};

// A language pair the engine cannot translate now, with the reason (engine.hello.pairsUnavailable).
struct PairState {
  std::string pair;
  bool available = false;
  vp::Error why;
};

class RequestQueue {
 public:
  static constexpr size_t kCap = 1024;
  bool tryPush(json req);                 // false when full or closed
  bool pop(json& req, int timeoutMs);     // false on timeout or when closed and empty
  void close();

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<json> q_;
  bool closed_ = false;
};

struct HistoryStep {
  std::string label;
  std::vector<std::pair<size_t, vp::CueRecord>> before, after;   // (position, record)
};

class Server {
 public:
  static constexpr size_t kHistoryCap = 500;          // steps
  static constexpr size_t kHistoryRecordCap = 200000;  // CueRecords held by all steps together
  static constexpr size_t kBatch = 20;          // cues per translate.cue event (and per engine call)
  static constexpr int64_t kProgressMs = 100;   // translate.progress at most this often
  static constexpr int64_t kTickMs = 1000;      // autosave check

  Server(Output& out, ServeOptions opt);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Reader thread.
  void onLine(std::string&& line);
  void onLineTooLong();
  void onEof();
  // Worker loop on the calling thread until engine.shutdown, EOF or a stop signal. Returns the exit code.
  int run();

 private:
  using Handler = json (Server::*)(const json& params);

  void sendOk(const json& id, json result);
  void sendError(const json& id, const vp::Error& e);
  void sendEvent(const char* name, json body);
  void requestStop();
  void handle(const json& req);
  void tick(bool force);
  void finish();

  // project helpers
  void requireProject() const;
  size_t position(const json& params, const char* key = "index") const;   // checked 0-based index
  void loadDocument(std::vector<std::string>& warnings);
  void installProject(vp::Project&& p, const std::string& path, const std::string& lockBase, bool takeLock,
                      std::vector<std::string>& warnings);
  void closeProject(bool discardAutosave);
  json projectJson() const;
  json statsJson() const;
  json cueView(size_t pos) const;
  CueViewCtx viewCtx() const;
  vp::rules::Options engineOptions() const;
  vp::rules::Context engineContext() const;
  vp::rules::CueInput cueInput(size_t pos) const;
  void noteChange();
  void pushHistory(HistoryStep&& step);
  void dropOldestStep();
  void refreshSettingsCache();
  bool writeAutosave();
  void addRecent(const std::string& path);
  const vp::lex::Lexicon* lexFor(vp::rules::Lang l) const;
  void runJob(int64_t jobId, std::vector<size_t> positions, vp::rules::Options opt, const char* prefix,
              std::vector<std::pair<std::string, vp::Error>> warnings);
  // engine wiring (C8): data folders, pairs, advisors of engines ii/iii, stored per-cue engine data
  void setupEngine();
  const std::vector<PairState>& pairStates();
  void requirePair(const std::string& pair);
  int adviseSense(const std::string& prompt, const std::vector<std::string>& options);
  vp::rules::Evidence adviseOnline(const std::string& lemma, const std::string& gloss);
  void jobWarning(const char* engine, const vp::Error& e);
  std::vector<vp::rules::TokenView> tokensOf(size_t pos) const;
  void retarget(size_t pos, const std::string& text);   // user text in place of the target: check, tokens, reasons
  void storeOutput(size_t pos, const vp::rules::CueOutput& out);
  bool orbergMode() const;
  bool probePair(const PairLangs& pl, vp::Error& why);  // one empty cue through the engine
  // Orbergise: the original-language file aligned to the cues (lang "" = detect), and forgetting it
  void loadOriginal(const std::string& path, const std::string& lang);
  void clearOriginal();
  std::vector<size_t> indicesParam(const json& p, bool* given) const;

  // commands (DESIGN §9 table)
  json cmdHello(const json&);
  json cmdSettingsGet(const json&);
  json cmdSettingsSet(const json&);
  json cmdProjectNew(const json&);
  json cmdProjectOpen(const json&);
  json cmdProjectRecover(const json&);
  json cmdProjectSave(const json&);
  json cmdProjectSaveAs(const json&);
  json cmdProjectClose(const json&);
  json cmdCuePage(const json&);
  json cmdCueGet(const json&);
  json cmdCueSet(const json&);
  json cmdCueChoose(const json&);
  json cmdCueReview(const json&);
  json cmdTranslateStart(const json&);
  json cmdOrbergiseStart(const json&);
  json cmdWordInspect(const json&);
  json cmdLemmaGet(const json&);
  json cmdNamesList(const json&);
  json cmdNamesSet(const json&);
  json cmdCorrectionsList(const json&);
  json cmdCorrectionsRemove(const json&);
  json cmdWordsList(const json&);
  json cmdExportWrite(const json&);
  json cmdExportPreview(const json&);
  json cmdModelStatus(const json&);
  json cmdModelLocate(const json&);
  json cmdModelTest(const json&);
  json cmdModelUnload(const json&);
  json cmdOnlineTest(const json&);
  json cmdHistoryUndo(const json&);
  json cmdHistoryRedo(const json&);
  json cmdEvalRun(const json&);
  json historyJson(const std::vector<size_t>& changed) const;
  ExportOptions exportOptions(const json& params) const;
  // engine ii (server_export.cpp): model file discovery, status and the config used for every load
  std::string modelPath() const;
  vp::llm::Config modelConfig() const;
  json modelStatusJson(bool hash);
  bool modelUsable(std::string& why);

  Output& out_;
  ServeOptions opt_;
  vp::Settings settings_;
  std::array<vp::lex::Lexicon, 4> lex_;      // la, grc, en, es
  std::array<bool, 4> lexOk_{};
  json lexInfo_ = json::array();
  std::unique_ptr<vp::rules::Engine> engine_;
  RequestQueue queue_;
  std::atomic<bool> stop_{false};
  std::atomic<int64_t> runningJob_{0}, cancelJob_{0};
  int64_t nextJob_ = 1;
  std::function<void()> pendingJob_;         // runs after the response of translate.start / orbergise.start
  int64_t lastTickMs_ = 0;
  int64_t autosaveDebounceMs_ = 5000;

  // the open project (worker thread only)
  bool hasProject_ = false;
  vp::Project project_;
  std::string path_;        // where it is saved ("" until the first save)
  std::string lockBase_;    // path_, or a file name under <data>/unsaved for a project never saved
  bool ownLock_ = false;
  vp::subs::Document doc_;
  vp::subs::Format format_ = vp::subs::Format::Srt;
  std::vector<std::string> sources_;
  PairLangs langs_;
  vp::Autosaver autosaver_;
  std::string lastAutosaveAt_;
  std::deque<HistoryStep> undo_, redo_;
  size_t historyRecords_ = 0;
  int nextCorrection_ = 1;
  double cpsLimit_ = 17;    // settings cps.adult, refreshed when settings change
  vp::llm::Model model_;    // engine ii: loaded only inside a job or model.test, unloaded after it / 60 s idle

  // engine wiring (C8)
  bool realEngine_ = false;
  std::string nlpDir_, curatedDir_;
  bool nlpEn_ = false, nlpEs_ = false;
  std::vector<std::pair<std::string, std::string>> samples_;   // (lang, path)
  std::vector<PairState> pairs_;
  bool pairsProbed_ = false;
  json lexTiers_ = json::object();                              // lang -> {t1, t2, t3}
  // the running job (worker thread): what engines ii/iii may do and what they said for the current batch
  int64_t jobId_ = 0;
  vp::llm::Advisors jobModel_;
  bool jobModelFailed_ = false, jobOnline_ = false, jobOnlineFailed_ = false;
  vp::rules::Lang jobTarget_ = vp::rules::Lang::La;
  JobFacts batchFacts_;
  // engine iii client, created on the first online question of a job when the settings allow it
  std::unique_ptr<vp::online::Transport> onlineTransport_;
  std::unique_ptr<vp::online::Wiktionary> onlineClient_;
  std::unique_ptr<vp::online::Throttle> mockThrottle_;
  std::string onlineMock_;                                      // VP_ONLINE_MOCK (tests only): "", "1", "disagree"
  std::atomic<uint64_t> mockCalls_{0};
  // Orbergise: the original-language cue texts aligned to the project's cues (cue.get .original)
  std::string originalPath_;
  std::string originalLang_ = "en";                             // "en" | "es" (given or detected)
  std::vector<std::string> originals_;
};

}  // namespace vpcli
