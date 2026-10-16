// Portable host logic of the Windows shell (see host_logic.h). No Windows headers here.
#include "host_logic.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace vp {
namespace shell {
namespace {

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void skipWs(std::string_view s, size_t& i) {
  while (i < s.size() && isWs(s[i])) { ++i; }
}

// Advances i past a string literal starting at s[i] == '"'. False if unterminated.
bool skipString(std::string_view s, size_t& i) {
  if (i >= s.size() || s[i] != '"') { return false; }
  ++i;
  while (i < s.size()) {
    // Jump to the next quote or backslash quickly (large cue pages).
    const char* base = s.data() + i;
    size_t rem = s.size() - i;
    const char* q = static_cast<const char*>(std::memchr(base, '"', rem));
    const char* b = static_cast<const char*>(std::memchr(base, '\\', q ? size_t(q - base) : rem));
    if (b) { i = size_t(b - s.data()) + 2; continue; }
    if (!q) { return false; }
    i = size_t(q - s.data()) + 1;
    return true;
  }
  return false;
}

// Advances i past one JSON value of any kind. Nesting is capped to stay bounded.
bool skipValue(std::string_view s, size_t& i) {
  skipWs(s, i);
  if (i >= s.size()) { return false; }
  char c = s[i];
  if (c == '"') { return skipString(s, i); }
  if (c == '{' || c == '[') {
    int depth = 0;
    while (i < s.size()) {
      char d = s[i];
      if (d == '"') {
        if (!skipString(s, i)) { return false; }
        continue;
      }
      if (d == '{' || d == '[') {
        if (++depth > 256) { return false; }
      } else if (d == '}' || d == ']') {
        if (--depth == 0) { ++i; return true; }
      }
      ++i;
    }
    return false;
  }
  // number, true, false, null
  size_t start = i;
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isWs(s[i])) { ++i; }
  return i > start;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

bool parseHex4(std::string_view s, size_t i, uint32_t& v) {
  if (i + 4 > s.size()) { return false; }
  v = 0;
  for (size_t k = i; k < i + 4; ++k) {
    int h = hexVal(s[k]);
    if (h < 0) { return false; }
    v = (v << 4) | uint32_t(h);
  }
  return true;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAlnum(char c) { return isAlpha(c) || (c >= '0' && c <= '9'); }

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) { return false; }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (lower(s[i]) != lower(prefix[i])) { return false; }
  }
  return true;
}

bool parseIntText(std::string_view raw, long long& out) {
  if (raw.empty() || raw.size() > 24) { return false; }
  char buf[32];
  std::memcpy(buf, raw.data(), raw.size());
  buf[raw.size()] = 0;
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(buf, &end, 10);
  if (errno != 0 || end != buf + raw.size()) { return false; }
  out = v;
  return true;
}

bool hasControl(std::string_view s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) { return true; }
  }
  return false;
}

// "<base>.autosave" -> "<base>"; "" when the name does not end that way.
std::string baseOfAutosave(std::string_view autosave) {
  constexpr std::string_view kSuffix = ".autosave";
  if (autosave.size() <= kSuffix.size() || autosave.substr(autosave.size() - kSuffix.size()) != kSuffix) {
    return std::string();
  }
  return std::string(autosave.substr(0, autosave.size() - kSuffix.size()));
}

}  // namespace

// ------------------------------------------------------------------------------- JSON

bool jsonFindRaw(std::string_view obj, std::string_view key, std::string_view& raw) {
  size_t i = 0;
  skipWs(obj, i);
  if (i >= obj.size() || obj[i] != '{') { return false; }
  ++i;
  for (;;) {
    skipWs(obj, i);
    if (i >= obj.size() || obj[i] == '}') { return false; }
    size_t ks = i;
    if (!skipString(obj, i)) { return false; }
    std::string_view k = obj.substr(ks + 1, i - ks - 2);
    skipWs(obj, i);
    if (i >= obj.size() || obj[i] != ':') { return false; }
    ++i;
    skipWs(obj, i);
    size_t vs = i;
    if (!skipValue(obj, i)) { return false; }
    // Keys the shell looks for never contain escapes, so a raw comparison is exact.
    if (k == key) {
      raw = obj.substr(vs, i - vs);
      return true;
    }
    skipWs(obj, i);
    if (i < obj.size() && obj[i] == ',') { ++i; continue; }
    return false;
  }
}

bool jsonArrayItems(std::string_view arr, std::vector<std::string_view>& items, size_t maxItems) {
  items.clear();
  size_t i = 0;
  skipWs(arr, i);
  if (i >= arr.size() || arr[i] != '[') { return false; }
  ++i;
  skipWs(arr, i);
  if (i < arr.size() && arr[i] == ']') { return true; }
  for (;;) {
    skipWs(arr, i);
    size_t vs = i;
    if (!skipValue(arr, i)) { return false; }
    if (items.size() >= maxItems) { return false; }
    items.push_back(arr.substr(vs, i - vs));
    skipWs(arr, i);
    if (i >= arr.size()) { return false; }
    if (arr[i] == ']') { return true; }
    if (arr[i] != ',') { return false; }
    ++i;
  }
}

bool jsonDecodeString(std::string_view s, std::string& out) {
  out.clear();
  if (s.size() < 2 || s.front() != '"' || s.back() != '"') { return false; }
  out.reserve(s.size() - 2);
  size_t i = 1, end = s.size() - 1;
  while (i < end) {
    char c = s[i];
    if (c != '\\') {
      if (static_cast<unsigned char>(c) < 0x20) { return false; }
      out.push_back(c);
      ++i;
      continue;
    }
    if (i + 1 >= end) { return false; }
    char e = s[i + 1];
    i += 2;
    switch (e) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        uint32_t cp = 0;
        if (i + 4 > end || !parseHex4(s, i, cp)) { return false; }
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t lo = 0;
          if (i + 6 <= end && s[i] == '\\' && s[i + 1] == 'u' && parseHex4(s, i + 2, lo) && lo >= 0xDC00 &&
              lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          } else {
            cp = 0xFFFD;  // unpaired surrogate
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        appendUtf8(out, cp);
        break;
      }
      default: return false;
    }
  }
  return true;
}

bool jsonGetString(std::string_view obj, std::string_view key, std::string& out) {
  std::string_view raw;
  if (!jsonFindRaw(obj, key, raw) || raw.empty() || raw.front() != '"') { return false; }
  return jsonDecodeString(raw, out);
}

bool jsonGetInt(std::string_view obj, std::string_view key, long long& out) {
  std::string_view raw;
  if (!jsonFindRaw(obj, key, raw)) { return false; }
  return parseIntText(raw, out);
}

bool jsonGetBool(std::string_view obj, std::string_view key, bool& out) {
  std::string_view raw;
  if (!jsonFindRaw(obj, key, raw)) { return false; }
  if (raw == "true") { out = true; return true; }
  if (raw == "false") { out = false; return true; }
  return false;
}

std::string jsonQuote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (u < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(u));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string jsonStringArray(const std::vector<std::string>& items) {
  std::string s = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) { s.push_back(','); }
    s += jsonQuote(items[i]);
  }
  s.push_back(']');
  return s;
}

// --------------------------------------------------------------------------- messages

bool parseWebRequest(std::string_view text, WebRequest& out) {
  out = WebRequest();
  if (!jsonGetInt(text, "id", out.id)) { return false; }
  if (!jsonGetString(text, "cmd", out.cmd) || out.cmd.empty() || out.cmd.size() > 64) { return false; }
  std::string_view params;
  if (jsonFindRaw(text, "params", params) && params != "null") {
    if (params.empty() || params.front() != '{') { return false; }
    out.params = params;
  } else {
    out.params = std::string_view("{}");
  }
  return true;
}

Route routeCommand(std::string_view cmd) {
  if (cmd == "dialog.openFile" || cmd == "dialog.saveFile" || cmd == "dialog.droppedFiles" ||
      cmd == "shell.revealFile" || cmd == "shell.openExternal") {
    return Route::Shell;
  }
  if (cmd.rfind("dialog.", 0) == 0 || cmd.rfind("shell.", 0) == 0) { return Route::Reject; }
  return Route::Engine;
}

EngineLine classifyEngineLine(std::string_view line) {
  EngineLine r;
  std::string_view raw;
  if (jsonFindRaw(line, "event", raw)) {
    if (jsonDecodeString(raw, r.event) && !r.event.empty()) { r.kind = EngineLine::Event; }
    return r;
  }
  if (jsonGetInt(line, "id", r.id)) {
    bool ok = false;
    r.ok = jsonGetBool(line, "ok", ok) && ok;
    r.kind = EngineLine::Response;
  } else if (jsonFindRaw(line, "id", raw) && raw == "null") {
    r.kind = EngineLine::Response;  // the engine's answer to a line it could not read
    r.id = 0;
  }
  return r;
}

std::string makeRequest(long long id, std::string_view cmd, std::string_view paramsJson) {
  std::string s = "{\"id\":" + std::to_string(id) + ",\"cmd\":" + jsonQuote(cmd) + ",\"params\":";
  s.append(paramsJson.empty() ? std::string_view("{}") : paramsJson);
  s.push_back('}');
  return s;
}

std::string makeOkResponse(long long id, std::string_view resultJson) {
  std::string s = "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":";
  s.append(resultJson.empty() ? std::string_view("{}") : resultJson);
  s.push_back('}');
  return s;
}

std::string makeErrorResponse(long long id, std::string_view code, std::string_view message, std::string_view hint) {
  return "{\"id\":" + std::to_string(id) + ",\"ok\":false,\"error\":{\"code\":" + jsonQuote(code) +
         ",\"message\":" + jsonQuote(message) + ",\"hint\":" + jsonQuote(hint) + "}}";
}

std::string makeEvent(std::string_view name, std::string_view fields) {
  std::string s = "{\"event\":" + jsonQuote(name);
  if (!fields.empty()) {
    s.push_back(',');
    s.append(fields);
  }
  s.push_back('}');
  return s;
}

// ------------------------------------------------------------------------- supervision

void Watchdog::reset(int64_t nowMs) {
  lastAlive_ = nowMs;
  lastPing_ = nowMs;
}

Watchdog::Action Watchdog::tick(int64_t nowMs) {
  if (nowMs - lastAlive_ >= timeout_) {
    reset(nowMs);
    return Action::Restart;
  }
  if (nowMs - lastPing_ >= interval_) {
    lastPing_ = nowMs;
    return Action::SendPing;
  }
  return Action::None;
}

bool RestartLimiter::allow(int64_t nowMs) {
  while (!times_.empty() && nowMs - times_.front() >= window_) { times_.pop_front(); }
  if (int(times_.size()) >= max_) { return false; }
  times_.push_back(nowMs);
  return true;
}

void SessionTracker::onRequest(long long id, std::string_view cmd, std::string_view params) {
  if (inFlight_.size() >= kMaxInFlight) { inFlight_.pop_front(); }
  Pending p{id, std::string(cmd), std::string()};
  std::string_view patch;
  if (cmd == "settings.set" && jsonFindRaw(params, "patch", patch) && !patch.empty() && patch.front() == '{') {
    p.patch.assign(patch.data(), patch.size());
  }
  inFlight_.push_back(std::move(p));
}

void SessionTracker::noteSettings(std::string_view s) {
  std::string v;
  if (jsonGetString(s, "lang", v) && !v.empty()) { lang_ = v; }
  if (jsonGetString(s, "theme", v) && !v.empty()) { theme_ = v; }
}

void SessionTracker::noteProject(std::string_view result, std::string_view cmd) {
  std::string_view project;
  std::string path, autosave;
  if (jsonFindRaw(result, "project", project) && !project.empty() && project.front() == '{') {
    jsonGetString(project, "path", path);  // null for an untitled project
    jsonGetString(project, "autosavePath", autosave);
  } else {
    jsonGetString(result, "path", path);
  }
  std::string base = !path.empty() ? path : baseOfAutosave(autosave);
  if (base.empty()) {
    if (cmd == "project.new") { recoverBase_.clear(); projectPath_.clear(); }
    return;
  }
  recoverBase_ = base;
  projectPath_ = path;
}

void SessionTracker::onResponse(long long id, bool ok, std::string_view line) {
  auto it = std::find_if(inFlight_.begin(), inFlight_.end(), [id](const Pending& p) { return p.id == id; });
  if (it == inFlight_.end()) { return; }
  Pending p = std::move(*it);
  inFlight_.erase(it);
  if (!ok) { return; }
  std::string_view result;
  if (!jsonFindRaw(line, "result", result)) { return; }
  if (p.cmd == "project.new" || p.cmd == "project.open" || p.cmd == "project.recover" || p.cmd == "project.save" ||
      p.cmd == "project.saveAs") {
    noteProject(result, p.cmd);
  } else if (p.cmd == "project.close") {
    recoverBase_.clear();
    projectPath_.clear();
  } else if (p.cmd == "settings.get" || p.cmd == "settings.set") {
    noteSettings(result);
  }
}

std::vector<long long> SessionTracker::onEngineRestart(std::vector<std::string>* replay) {
  std::vector<long long> ids;
  ids.reserve(inFlight_.size());
  for (Pending& p : inFlight_) {
    ids.push_back(p.id);
    if (replay && !p.patch.empty()) { replay->push_back(std::move(p.patch)); }
  }
  inFlight_.clear();
  return ids;
}

RestartSequence::Step RestartSequence::start(const SessionTracker& t, const std::vector<std::string>& replay,
                                             int64_t nowMs) {
  Step s;
  active_ = true;
  startMs_ = nowMs;
  base_ = t.recoverBase();
  path_ = t.projectPath();
  for (const std::string& patch : replay) {
    s.send.push_back(makeRequest(kSettingsId, "settings.set", "{\"patch\":" + patch + "}"));
  }
  s.send.push_back(makeRequest(kSettingsId, "settings.get", "{}"));
  if (base_.empty()) {
    Step done = finish(false, false);
    s.event = std::move(done.event);
    return s;
  }
  s.send.push_back(makeRequest(kRecoverId, "project.recover", "{\"path\":" + jsonQuote(base_) + "}"));
  return s;
}

RestartSequence::Step RestartSequence::onResponse(long long id, bool ok, int64_t) {
  Step s;
  if (!active_) { return s; }
  if (id == kRecoverId) {
    if (ok) { return finish(true, false); }
    // Nothing to recover (no autosave since the last save) or the autosave is unreadable:
    // reopen the saved file instead; an untitled project without autosave is gone.
    if (path_.empty()) { return finish(false, false); }
    s.send.push_back(makeRequest(kReopenId, "project.open", "{\"path\":" + jsonQuote(path_) + "}"));
    return s;
  }
  if (id == kReopenId) { return finish(false, ok); }
  return s;  // kSettingsId: nothing to decide
}

RestartSequence::Step RestartSequence::finish(bool recovered, bool reopened) {
  Step s;
  active_ = false;
  const std::string& shown = !path_.empty() ? path_ : base_;
  std::string fields = std::string("\"recovered\":") + (recovered ? "true" : "false") +
                       ",\"reopened\":" + (reopened ? "true" : "false") + ",\"path\":" +
                       (shown.empty() || (!recovered && !reopened) ? std::string("null") : jsonQuote(shown));
  s.event = makeEvent("engine.restarted", fields);
  return s;
}

// ----------------------------------------------------------------------------- placement

std::string placementToJson(const Placement& p) {
  return "{\"window.x\":" + std::to_string(p.x) + ",\"window.y\":" + std::to_string(p.y) + ",\"window.w\":" +
         std::to_string(p.w) + ",\"window.h\":" + std::to_string(p.h) + ",\"window.maximized\":" +
         (p.maximized ? "true" : "false") + "}\n";
}

bool placementFromJson(std::string_view json, Placement& out) {
  long long x, y, w, h;
  if (!jsonGetInt(json, "window.x", x) || !jsonGetInt(json, "window.y", y) || !jsonGetInt(json, "window.w", w) ||
      !jsonGetInt(json, "window.h", h)) {
    return false;
  }
  const long long lim = 1 << 20;
  if (w <= 0 || h <= 0 || w > lim || h > lim || x < -lim || x > lim || y < -lim || y > lim) { return false; }
  Placement p;
  p.x = int(x);
  p.y = int(y);
  p.w = int(w);
  p.h = int(h);
  bool m = false;
  p.maximized = jsonGetBool(json, "window.maximized", m) && m;
  out = p;
  return true;
}

Placement defaultPlacement(const Rect& work, int w, int h) {
  int ww = std::max(1, work.right - work.left), wh = std::max(1, work.bottom - work.top);
  Placement p;
  p.w = std::min(w, ww);
  p.h = std::min(h, wh);
  p.x = work.left + (ww - p.w) / 2;
  p.y = work.top + (wh - p.h) / 2;
  return p;
}

Placement sanitizePlacement(Placement p, const Rect& work, int minW, int minH) {
  int ww = std::max(1, work.right - work.left), wh = std::max(1, work.bottom - work.top);
  p.w = std::min(std::max(p.w, std::min(minW, ww)), ww);
  p.h = std::min(std::max(p.h, std::min(minH, wh)), wh);
  p.x = std::min(std::max(p.x, work.left), work.right - p.w);
  p.y = std::min(std::max(p.y, work.top), work.bottom - p.h);
  return p;
}

int dipToPx(int dip, unsigned dpi) {
  if (dpi == 0) { dpi = 96; }
  long long v = (long long)dip * (long long)dpi;
  return int(v >= 0 ? (v + 48) / 96 : -((-v + 48) / 96));
}

Size outerSizeForClient(Size clientDip, unsigned dpi, Size frame, Size work) {
  Size s;
  s.w = dipToPx(clientDip.w, dpi) + std::max(0, frame.w);
  s.h = dipToPx(clientDip.h, dpi) + std::max(0, frame.h);
  if (work.w > 0) { s.w = std::min(s.w, work.w); }
  if (work.h > 0) { s.h = std::min(s.h, work.h); }
  return s;
}

// ---------------------------------------------------------------------------------- misc

bool isAppUri(std::string_view uri) { return startsWithNoCase(uri, kAppOrigin); }

bool isSafeExternalUrl(std::string_view url) {
  if (url.size() > 2048) { return false; }
  std::string_view rest;
  if (startsWithNoCase(url, "https://")) {
    rest = url.substr(8);
  } else if (startsWithNoCase(url, "http://")) {
    rest = url.substr(7);
  } else {
    return false;
  }
  if (rest.empty() || rest.front() == '/' || rest.front() == '?' || rest.front() == '#') { return false; }
  for (char c : url) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`' || c == '^' ||
        c == '{' || c == '}' || c == '|') {
      return false;
    }
  }
  return true;
}

bool normaliseRevealPath(std::string_view in, std::string& out) {
  out.clear();
  if (in.size() < 3 || in.size() > 32767 || !isValidUtf8(in)) { return false; }
  std::string p(in);
  std::replace(p.begin(), p.end(), '/', '\\');
  size_t rootEnd = 0;  // index of the first character after the root ("C:\" or "\\server\share\")
  if (isAlpha(p[0]) && p[1] == ':' && p[2] == '\\') {
    rootEnd = 3;
  } else if (p[0] == '\\' && p[1] == '\\') {
    size_t server = p.find('\\', 2);
    if (server == std::string::npos || server == 2) { return false; }
    size_t share = p.find('\\', server + 1);
    if (share == server + 1) { return false; }
    rootEnd = share == std::string::npos ? p.size() : share + 1;
  } else {
    return false;
  }
  for (size_t i = 0; i < p.size(); ++i) {
    char c = p[i];
    if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' ||
        c == '*' || (c == ':' && i != 1)) {
      return false;
    }
  }
  size_t start = rootEnd;
  while (start < p.size()) {
    size_t slash = p.find('\\', start);
    size_t end = slash == std::string::npos ? p.size() : slash;
    std::string_view seg(p.data() + start, end - start);
    if (seg.empty() || seg == "." || seg == "..") { return false; }
    if (slash == std::string::npos || slash + 1 == p.size()) { break; }
    start = slash + 1;
  }
  out = std::move(p);
  return true;
}

bool filePathFromFileUri(std::string_view uri, std::string& out) {
  out.clear();
  if (!startsWithNoCase(uri, "file://") || uri.size() > 3 * 32767) { return false; }
  std::string_view rest = uri.substr(7);
  size_t cut = rest.find_first_of("?#");
  if (cut != std::string_view::npos) { rest = rest.substr(0, cut); }
  if (startsWithNoCase(rest, "localhost/")) { rest = rest.substr(9); }
  std::string decoded;
  for (size_t i = 0; i < rest.size(); ++i) {
    char c = rest[i];
    if (c == '%') {
      if (i + 2 >= rest.size()) { return false; }
      int hi = hexVal(rest[i + 1]), lo = hexVal(rest[i + 2]);
      if (hi < 0 || lo < 0) { return false; }
      decoded.push_back(char(hi * 16 + lo));
      i += 2;
    } else {
      decoded.push_back(c);
    }
  }
  std::string path;
  if (!decoded.empty() && decoded[0] == '/') {
    path = decoded.substr(1);  // "/C:/x" -> "C:/x"
  } else {
    path = "//" + decoded;  // host form: "server/share/x" -> "//server/share/x"
  }
  return normaliseRevealPath(path, out);
}

std::string extensionOf(std::string_view path) {
  size_t sep = path.find_last_of("\\/");
  size_t dot = path.rfind('.');
  if (dot == std::string_view::npos || (sep != std::string_view::npos && dot < sep) || dot + 1 >= path.size()) {
    return std::string();
  }
  std::string e(path.substr(dot + 1));
  for (char& c : e) { c = lower(c); }
  return e;
}

bool isOpenableFile(std::string_view path) {
  std::string e = extensionOf(path);
  return e == "vpoeta" || e == "srt" || e == "vtt" || e == "ass" || e == "ssa" || e == "txt";
}

void splitDirName(std::string_view path, std::string& dir, std::string& name) {
  size_t sep = path.find_last_of("\\/");
  if (sep == std::string_view::npos) {
    dir.clear();
    name.assign(path.data(), path.size());
    return;
  }
  dir.assign(path.data(), sep);
  if (dir.size() == 2 && dir[1] == ':') { dir.push_back('\\'); }  // "C:" -> "C:\"
  name.assign(path.data() + sep + 1, path.size() - sep - 1);
}

std::string quoteWindowsArg(std::string_view arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) { return std::string(arg); }
  std::string out = "\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');  // they precede the closing quote
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

namespace {
// One extension ("srt", ".srt", or with patterns=true "*.srt" / "*.*") -> "srt" or "*". False if invalid.
bool normaliseExtension(std::string ext, bool patterns, std::string& out) {
  if (patterns) {
    if (ext == "*.*" || ext == "*") {
      ext = "*";
    } else if (ext.size() > 2 && ext[0] == '*' && ext[1] == '.') {
      ext = ext.substr(2);
    } else {
      return false;
    }
  } else if (!ext.empty() && ext[0] == '.') {
    ext = ext.substr(1);
  }
  if (ext.empty() || ext.size() > 16) { return false; }
  if (ext != "*") {
    for (char& c : ext) {
      if (!isAlnum(c)) { return false; }
      c = lower(c);
    }
  }
  out = std::move(ext);
  return true;
}

bool appendExtensions(std::string_view list, bool patterns, FileFilter& f) {
  std::vector<std::string_view> exts;
  if (!jsonArrayItems(list, exts, 32) || exts.empty()) { return false; }
  for (std::string_view e : exts) {
    std::string raw, ext;
    if (!jsonDecodeString(e, raw) || !normaliseExtension(std::move(raw), patterns, ext)) { return false; }
    if (!f.pattern.empty()) { f.pattern.push_back(';'); }
    f.pattern += ext == "*" ? std::string("*.*") : "*." + ext;
    if (f.firstExt.empty() && ext != "*") { f.firstExt = ext; }
  }
  return true;
}
}  // namespace

bool parseFilters(std::string_view raw, std::vector<FileFilter>& out) {
  out.clear();
  std::vector<std::string_view> items;
  if (!jsonArrayItems(raw, items, 32)) { return false; }
  if (!items.empty() && items[0].front() == '"') {
    // ["srt","vtt"]: one filter without a name (the shell names it and adds "All files").
    FileFilter f;
    if (!appendExtensions(raw, false, f)) { return false; }
    out.push_back(std::move(f));
    return true;
  }
  if (items.size() > 16) { return false; }
  for (std::string_view item : items) {
    FileFilter f;
    if (!jsonGetString(item, "name", f.name) || f.name.empty() || f.name.size() > 256 || hasControl(f.name)) {
      return false;
    }
    std::string_view list;
    bool patterns = false;
    if (!jsonFindRaw(item, "extensions", list)) {
      if (!jsonFindRaw(item, "patterns", list)) { return false; }
      patterns = true;
    }
    if (!appendExtensions(list, patterns, f)) { return false; }
    out.push_back(std::move(f));
  }
  return true;
}

CommandLine parseCommandLine(const std::vector<std::string>& args) {
  CommandLine cl;
  for (const std::string& a : args) {
    if (a == "--devtools") {
      cl.devtools = true;
      continue;
    }
    if (a.rfind("--", 0) == 0) { continue; }
    if (cl.files.size() < kMaxForwardPaths && isOpenableFile(a) && !hasControl(a)) { cl.files.push_back(a); }
  }
  return cl;
}

bool isValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; continue; }
    size_t n;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
    else { return false; }
    for (size_t k = 1; k <= n; ++k) {
      if (i + k >= s.size()) { return false; }
      unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) { return false; }
      cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += n + 1;
  }
  return true;
}

std::string encodeOpenPayload(const std::vector<std::string>& paths) {
  std::string out;
  for (const std::string& p : paths) {
    if (p.empty() || hasControl(p)) { continue; }
    if (!out.empty()) { out.push_back('\n'); }
    out += p;
  }
  return out;
}

bool decodeOpenPayload(uint32_t magic, const void* data, size_t size, std::vector<std::string>& paths) {
  paths.clear();
  if (magic != kCopyDataMagic || !data || size == 0 || size > kMaxCopyDataBytes) { return false; }
  std::string_view s(static_cast<const char*>(data), size);
  while (!s.empty() && s.back() == '\0') { s.remove_suffix(1); }  // tolerate a terminator
  if (s.empty() || !isValidUtf8(s)) { return false; }
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    std::string_view line = s.substr(start, (nl == std::string_view::npos ? s.size() : nl) - start);
    if (line.empty() || line.size() > 32767 * 3 || hasControl(line) || paths.size() >= kMaxForwardPaths) {
      paths.clear();
      return false;
    }
    paths.emplace_back(line);
    if (nl == std::string_view::npos) { break; }
    start = nl + 1;
  }
  return !paths.empty();
}

bool themeIsDark(std::string_view theme, bool systemDark) {
  if (theme == "dark") { return true; }
  if (theme == "light") { return false; }
  return systemDark;
}

std::string uiLanguage(std::string_view lang) {
  return lang.size() >= 2 && lower(lang[0]) == 'e' && lower(lang[1]) == 's' ? "es-MX" : "en-US";
}

}  // namespace shell
}  // namespace vp
