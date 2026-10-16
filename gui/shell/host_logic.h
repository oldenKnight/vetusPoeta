// Portable host logic of the Windows shell: no Windows headers, so it is unit-tested on Linux
// (engine/tests/test_shell_logic.cpp) and shared by the Win32 code. DESIGN.md §9 (protocol and the
// shell-handled commands), §13 (UI served at https://app.vetuspoeta/), PREDESIGN 4.8 (recovery).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vp {
namespace shell {

// ---------------------------------------------------------------------------------------
// Minimal JSON reading: just enough to route messages without parsing large payloads. Values
// are located by walking the top-level object (strings, nesting and escapes are honoured),
// never by substring search. Nothing here throws.
// ---------------------------------------------------------------------------------------

// Finds `key` among the top-level members of the JSON object `obj` and returns the raw text of
// its value (e.g. `"abc"`, `12`, `{...}`). False if `obj` is not an object or has no such key.
bool jsonFindRaw(std::string_view obj, std::string_view key, std::string_view& raw);
// Splits a JSON array into the raw text of its items (at most maxItems). False if not an array
// or malformed, or if it has more than maxItems items.
bool jsonArrayItems(std::string_view arr, std::vector<std::string_view>& items, size_t maxItems);
// Decodes a JSON string literal (with quotes) to UTF-8. False on malformed input.
bool jsonDecodeString(std::string_view literal, std::string& out);
bool jsonGetString(std::string_view obj, std::string_view key, std::string& out);
bool jsonGetInt(std::string_view obj, std::string_view key, long long& out);
bool jsonGetBool(std::string_view obj, std::string_view key, bool& out);
// Returns a JSON string literal (with quotes). Input is UTF-8 and is passed through.
std::string jsonQuote(std::string_view utf8);
std::string jsonStringArray(const std::vector<std::string>& utf8);

// ---------------------------------------------------------------------------------------
// Protocol messages (DESIGN §9 framing)
// ---------------------------------------------------------------------------------------

// Ids the shell uses for its own requests to the engine (the UI only uses ids >= 1).
constexpr long long kPingId = -1;
constexpr long long kRecoverId = -2;
constexpr long long kShutdownId = -3;
constexpr long long kReopenId = -4;
constexpr long long kSettingsId = -5;

struct WebRequest {
  long long id = 0;
  std::string cmd;
  std::string_view params;  // raw JSON object text ("{}" when absent); views the input
};
// Parses a request from the page: {"id":<int>,"cmd":"<name>","params":{...}}.
bool parseWebRequest(std::string_view text, WebRequest& out);

// Where a request from the page goes: the shell answers dialog.openFile, dialog.saveFile,
// dialog.droppedFiles (paths of File objects posted with the message), shell.revealFile and
// shell.openExternal itself; any other dialog.* / shell.* name is refused (never reaches the
// engine); everything else is forwarded to vpengine unchanged.
enum class Route { Shell, Engine, Reject };
Route routeCommand(std::string_view cmd);

struct EngineLine {
  enum Kind { Response, Event, Invalid } kind = Invalid;
  long long id = 0;   // Response
  bool ok = false;    // Response
  std::string event;  // Event
};
EngineLine classifyEngineLine(std::string_view line);

std::string makeRequest(long long id, std::string_view cmd, std::string_view paramsJson);
std::string makeOkResponse(long long id, std::string_view resultJson);
std::string makeErrorResponse(long long id, std::string_view code, std::string_view message, std::string_view hint);
// `fields` is a comma-separated list of JSON members without braces, may be empty.
std::string makeEvent(std::string_view name, std::string_view fields);

// ---------------------------------------------------------------------------------------
// Line splitter for the engine's stdout: handles partial reads and CRLF, and caps the length
// of a single line (longer lines are dropped whole and counted).
// ---------------------------------------------------------------------------------------
class LineSplitter {
 public:
  static constexpr size_t kDefaultMaxLine = size_t(64) << 20;  // 64 MiB
  explicit LineSplitter(size_t maxLine = kDefaultMaxLine) : max_(maxLine) {}

  // Calls onLine(std::string_view) for every complete, non-empty line in data.
  template <class F>
  void feed(const char* data, size_t n, F&& onLine) {
    while (n > 0) {
      const char* nl = static_cast<const char*>(std::memchr(data, '\n', n));
      size_t take = nl ? size_t(nl - data) : n;
      if (discarding_) {
        if (!nl) { return; }
        discarding_ = false;
      } else if (buf_.size() + take > max_) {
        ++dropped_;
        releaseBuffer();
        discarding_ = !nl;
      } else if (nl && buf_.empty()) {
        deliver(std::string_view(data, take), onLine);  // no copy on the common path
      } else {
        buf_.append(data, take);
        if (nl) {
          deliver(std::string_view(buf_), onLine);
          if (buf_.capacity() > kKeepCapacity) { releaseBuffer(); } else { buf_.clear(); }
        }
      }
      if (!nl) { return; }
      n -= take + 1;
      data = nl + 1;
    }
  }
  size_t dropped() const { return dropped_; }
  size_t pendingBytes() const { return buf_.size(); }
  void reset() { releaseBuffer(); discarding_ = false; }

 private:
  static constexpr size_t kKeepCapacity = size_t(1) << 20;  // keep up to 1 MiB for reuse
  template <class F>
  static void deliver(std::string_view line, F& onLine) {
    if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
    if (!line.empty()) { onLine(line); }
  }
  void releaseBuffer() { std::string().swap(buf_); }
  std::string buf_;
  size_t max_;
  size_t dropped_ = 0;
  bool discarding_ = false;
};

// ---------------------------------------------------------------------------------------
// Engine supervision (injected clock: every time argument is a monotonic millisecond count)
// ---------------------------------------------------------------------------------------

// Pings every `intervalMs`; the engine is hung when nothing came back for `timeoutMs` (the
// engine's reader thread answers engine.ping within 100 ms even during a job, so a busy engine
// still answers).
class Watchdog {
 public:
  enum class Action { None, SendPing, Restart };
  explicit Watchdog(int64_t intervalMs = 2000, int64_t timeoutMs = 10000)
      : interval_(intervalMs), timeout_(timeoutMs) {}
  void reset(int64_t nowMs);      // a (re)started engine counts as alive at nowMs
  Action tick(int64_t nowMs);     // call often (the shell ticks every 500 ms)
  void onPong(int64_t nowMs) { lastAlive_ = nowMs; }
  int64_t silentForMs(int64_t nowMs) const { return nowMs - lastAlive_; }

 private:
  int64_t interval_, timeout_;
  int64_t lastAlive_ = 0;
  int64_t lastPing_ = 0;
};

// At most `maxRestarts` within `windowMs` (DESIGN: 3 in 60 s, then a native error).
class RestartLimiter {
 public:
  explicit RestartLimiter(int maxRestarts = 3, int64_t windowMs = 60000) : max_(maxRestarts), window_(windowMs) {}
  bool allow(int64_t nowMs);  // records the restart when allowed
  void clear() { times_.clear(); }

 private:
  int max_;
  int64_t window_;
  std::deque<int64_t> times_;  // never longer than max_
};

// Follows the traffic between UI and engine: requests in flight, the open project (the path to
// pass to project.recover after a restart), the UI language and theme from settings results, and
// settings.set patches that were in flight when the engine died (they are replayed).
class SessionTracker {
 public:
  static constexpr size_t kMaxInFlight = 4096;
  void onRequest(long long id, std::string_view cmd, std::string_view params);
  void onResponse(long long id, bool ok, std::string_view line);
  // Reads lang/theme from a settings object (settings.get/set result, or settings.json).
  void noteSettings(std::string_view settingsObject);
  // Forgets everything in flight and returns those request ids (they will never be answered);
  // `replay` receives the `patch` objects of settings.set requests among them, oldest first.
  std::vector<long long> onEngineRestart(std::vector<std::string>* replay);
  bool busy() const { return !inFlight_.empty(); }
  size_t inFlight() const { return inFlight_.size(); }
  bool hasProject() const { return !recoverBase_.empty(); }
  // Base path whose "<base>.autosave" holds the newest state (the project file, or the untitled
  // project's base under <data>/unsaved/); empty when no project is open.
  const std::string& recoverBase() const { return recoverBase_; }
  // The saved project file ("" for an untitled project): project.open fallback.
  const std::string& projectPath() const { return projectPath_; }
  const std::string& lang() const { return lang_; }
  const std::string& theme() const { return theme_; }
  void setProject(std::string base, std::string path) { recoverBase_ = std::move(base); projectPath_ = std::move(path); }

 private:
  struct Pending { long long id; std::string cmd; std::string patch; };
  void noteProject(std::string_view result, std::string_view cmd);
  std::deque<Pending> inFlight_;
  std::string recoverBase_, projectPath_, lang_, theme_;
};

// After an engine restart: replay settings, recover the project, then tell the UI. Pure state
// machine; the Win32 side sends the lines it returns and feeds back the shell-id responses.
class RestartSequence {
 public:
  struct Step {
    std::vector<std::string> send;  // request lines for the engine, in order
    std::string event;              // non-empty: post this event to the UI, the sequence is over
  };
  static constexpr int64_t kTimeoutMs = 120000;  // project.recover of a big project
  // Starts a sequence for a fresh engine. `replay`: settings patches to re-send.
  Step start(const SessionTracker& t, const std::vector<std::string>& replay, int64_t nowMs);
  // A response with a shell id (kSettingsId, kRecoverId, kReopenId) arrived.
  Step onResponse(long long id, bool ok, int64_t nowMs);
  // Gives up when the engine does not answer in time (the caller restarts the engine again).
  bool timedOut(int64_t nowMs) const { return active_ && nowMs - startMs_ > kTimeoutMs; }
  bool active() const { return active_; }
  void cancel() { active_ = false; }

 private:
  Step finish(bool recovered, bool reopened);
  bool active_ = false;
  int64_t startMs_ = 0;
  std::string base_, path_;
};

// ---------------------------------------------------------------------------------------
// Log rotation: engine.log, engine.log.1 ... engine.log.<keep>
// ---------------------------------------------------------------------------------------
constexpr uint64_t kLogCapBytes = uint64_t(2) << 20;  // 2 MB
constexpr int kLogKeep = 2;
constexpr size_t kLogLineCap = size_t(16) << 10;  // one shell note in the log, at most
inline bool logNeedsRotation(uint64_t currentSize, uint64_t incoming, uint64_t cap = kLogCapBytes) {
  return currentSize > 0 && currentSize + incoming > cap;
}
// Ordered file operations for one rotation: (from, to); to empty = delete `from`.
template <class Str>
std::vector<std::pair<Str, Str>> logRotationPlan(const Str& base, int keep = kLogKeep) {
  std::vector<std::pair<Str, Str>> ops;
  auto name = [&](int i) {
    Str s = base;
    s.push_back(typename Str::value_type('.'));
    for (char c : std::to_string(i)) { s.push_back(typename Str::value_type(c)); }
    return s;
  };
  if (keep <= 0) { ops.emplace_back(base, Str()); return ops; }
  ops.emplace_back(name(keep), Str());
  for (int i = keep - 1; i >= 1; --i) { ops.emplace_back(name(i), name(i + 1)); }
  ops.emplace_back(base, name(1));
  return ops;
}

// ---------------------------------------------------------------------------------------
// Window state (<data>/window.json, keys window.x/y/w/h/maximized) and DPI-aware sizing
// ---------------------------------------------------------------------------------------
struct Rect { int left = 0, top = 0, right = 0, bottom = 0; };
struct Placement { int x = 0, y = 0, w = 0, h = 0; bool maximized = false; };
std::string placementToJson(const Placement& p);
bool placementFromJson(std::string_view json, Placement& out);
Placement defaultPlacement(const Rect& work, int w, int h);  // centred, never larger than work
// Clamps a restored placement so the window fits and is visible in `work`.
Placement sanitizePlacement(Placement p, const Rect& work, int minW, int minH);
struct Size { int w = 0, h = 0; };
int dipToPx(int dip, unsigned dpi);  // rounds like MulDiv; dpi 0 counts as 96
// Outer window size whose client area is `clientDip` at `dpi`, clamped to the work area.
Size outerSizeForClient(Size clientDip, unsigned dpi, Size frame, Size work);

// ---------------------------------------------------------------------------------------
// URLs, paths, files
// ---------------------------------------------------------------------------------------
constexpr const char* kAppOrigin = "https://app.vetuspoeta/";
bool isAppUri(std::string_view uri);
// http(s) only, a host, no whitespace/control characters/quotes, at most 2048 characters.
bool isSafeExternalUrl(std::string_view url);
// shell.revealFile: an absolute Windows path (drive "C:\..." or UNC "\\server\share\..."),
// no control characters, no wildcard or redirection characters, no "." / ".." segments,
// at most 32,767 characters. Forward slashes are accepted and become backslashes.
bool normaliseRevealPath(std::string_view path, std::string& out);
// file:///C:/a%20b/c.srt -> C:\a b\c.srt; file://server/share/x -> \\server\share\x.
// Used for files dropped on the window (WebView2 navigates to them when the page ignores the drop).
bool filePathFromFileUri(std::string_view uri, std::string& out);
std::string extensionOf(std::string_view path);  // lower-case, without the dot ("" if none)
// Files the app opens: .vpoeta projects, .srt/.vtt/.ass/.ssa subtitles, .txt texts.
bool isOpenableFile(std::string_view path);
// Splits "C:\\dir\\name.srt" into ("C:\\dir", "name.srt"); "C:" becomes "C:\\".
void splitDirName(std::string_view path, std::string& dir, std::string& name);
// Quotes one argument for CreateProcess so CommandLineToArgvW returns it unchanged.
std::string quoteWindowsArg(std::string_view arg);

// dialog.openFile / dialog.saveFile `filters`: [{"name":"Subtitles","extensions":["srt","vtt"]}]
// (also accepted: "patterns":["*.srt"]), at most 16 filters; or a plain list of extensions
// ["srt","vtt"], which gives one filter with an empty name (the shell names it). Extensions are
// 1-16 letters/digits, at most 32 per filter.
struct FileFilter {
  std::string name;     // UTF-8, shown as given (the UI sends translated names)
  std::string pattern;  // "*.srt;*.vtt"
  std::string firstExt; // "srt"
};
bool parseFilters(std::string_view filtersRaw, std::vector<FileFilter>& out);

// ---------------------------------------------------------------------------------------
// Command line and single instance
// ---------------------------------------------------------------------------------------
struct CommandLine {
  bool devtools = false;
  std::vector<std::string> files;  // openable files in argument order (at most kMaxForwardPaths)
};
constexpr size_t kMaxForwardPaths = 16;
CommandLine parseCommandLine(const std::vector<std::string>& argsUtf8);  // argv[0] excluded

// WM_COPYDATA from a second instance: dwData = kCopyDataMagic, payload = UTF-8 paths joined by
// '\n' (Windows paths never contain control characters). decode validates everything.
constexpr uint32_t kCopyDataMagic = 0x31504F56u;  // "VOP1" little-endian
constexpr size_t kMaxCopyDataBytes = 256u << 10;
std::string encodeOpenPayload(const std::vector<std::string>& pathsUtf8);
bool decodeOpenPayload(uint32_t magic, const void* data, size_t size, std::vector<std::string>& pathsUtf8);
bool isValidUtf8(std::string_view s);

// ---------------------------------------------------------------------------------------
// Theme and language
// ---------------------------------------------------------------------------------------
// Settings theme "dark" -> true, "light" -> false, anything else ("auto") -> the system's.
bool themeIsDark(std::string_view theme, bool systemDark);
// "es-MX" when lang starts with "es", else "en-US" (the two UI languages, DESIGN 9.1).
std::string uiLanguage(std::string_view lang);

}  // namespace shell
}  // namespace vp
