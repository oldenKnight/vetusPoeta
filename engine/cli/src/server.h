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
#include "vp/project.h"
#include "vp/result.h"
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
  std::string dataDir;      // settings.json, unsaved autosaves
  std::string lexiconDir;   // latin.vpl, greek.vpl, english.vpl, spanish.vpl
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
  void runJob(int64_t jobId, std::vector<size_t> positions, vp::rules::Options opt, const char* prefix);

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
  json cmdOnlineTest(const json&);
  json cmdHistoryUndo(const json&);
  json cmdHistoryRedo(const json&);
  json cmdEvalRun(const json&);
  json historyJson(const std::vector<size_t>& changed) const;
  ExportOptions exportOptions(const json& params) const;

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
};

}  // namespace vpcli
