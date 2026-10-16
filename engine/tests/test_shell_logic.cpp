// Tests for the portable host logic of the Windows shell (gui/shell/host_logic.h): framing, routing,
// supervision with an injected clock, restart sequence, paths and URLs, WM_COPYDATA payload,
// window state. The Win32 parts are smoke-tested by the owner on Windows (gui/shell/README.md).
#include <doctest.h>

#include <string>
#include <vector>

#include "host_logic.h"

using namespace vp::shell;

namespace {
std::vector<std::string> feedAll(LineSplitter& sp, const std::vector<std::string>& chunks) {
  std::vector<std::string> lines;
  for (const std::string& c : chunks) {
    sp.feed(c.data(), c.size(), [&](std::string_view l) { lines.emplace_back(l); });
  }
  return lines;
}
std::string cmdOf(const std::string& line) {
  WebRequest r;
  return parseWebRequest(line, r) ? r.cmd : std::string("?");
}
std::string paramsOf(const std::string& line) {
  WebRequest r;
  return parseWebRequest(line, r) ? std::string(r.params) : std::string("?");
}
}  // namespace

TEST_CASE("shell: line splitter handles partial reads, CRLF and empty lines") {
  LineSplitter sp;
  auto lines = feedAll(sp, {"{\"a\":", "1}\n{\"b\"", ":2}\r\n\n\r\n{\"c\":3}", "\n{\"d\""});
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "{\"a\":1}");
  CHECK(lines[1] == "{\"b\":2}");
  CHECK(lines[2] == "{\"c\":3}");
  CHECK(sp.pendingBytes() == 4);
  lines = feedAll(sp, {":4}\n"});
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "{\"d\":4}");
  CHECK(sp.pendingBytes() == 0);

  std::string text = "{\"id\":1,\"s\":\"x\\ny\"}\n{\"event\":\"e\"}\n";
  LineSplitter a, b;
  std::vector<std::string> la, lb;
  a.feed(text.data(), text.size(), [&](std::string_view l) { la.emplace_back(l); });
  for (char c : text) { b.feed(&c, 1, [&](std::string_view l) { lb.emplace_back(l); }); }
  CHECK(la == lb);
  CHECK(la.size() == 2);
}

TEST_CASE("shell: over-long lines are dropped whole, later lines survive") {
  LineSplitter sp(8);
  auto lines = feedAll(sp, {"12345", "6789", "abc\nok\n", "0123456789\nfine\n", "12345678\n"});
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "ok");
  CHECK(lines[1] == "fine");
  CHECK(lines[2] == "12345678");
  CHECK(sp.dropped() == 2);
  CHECK(sp.pendingBytes() == 0);
}

TEST_CASE("shell: json reads top-level members only, decodes escapes") {
  std::string_view j = R"({"params":{"cmd":"inner","path":"x"},"cmd":"project.open","id":12})";
  std::string s;
  REQUIRE(jsonGetString(j, "cmd", s));
  CHECK(s == "project.open");
  long long id = 0;
  REQUIRE(jsonGetInt(j, "id", id));
  CHECK(id == 12);
  CHECK_FALSE(jsonGetString(j, "path", s));
  std::string_view raw;
  REQUIRE(jsonFindRaw(j, "params", raw));
  CHECK(raw == R"({"cmd":"inner","path":"x"})");
  CHECK_FALSE(jsonFindRaw("not json", "a", raw));
  CHECK_FALSE(jsonFindRaw(R"({"a":"unterminated)", "a", raw));
  CHECK_FALSE(jsonFindRaw(R"({"a":1,"b":)", "b", raw));

  REQUIRE(jsonGetString(R"({"k":"C:\\Users\\Ana \"x\"\n\u00e9\ud83d\ude00"})", "k", s));
  CHECK(s == "C:\\Users\\Ana \"x\"\n\xC3\xA9\xF0\x9F\x98\x80");
  REQUIRE(jsonGetString(R"({"k":"\ud800x"})", "k", s));
  CHECK(s == "\xEF\xBF\xBDx");
  CHECK_FALSE(jsonGetString(R"({"k":"\q"})", "k", s));
  CHECK_FALSE(jsonGetString(R"({"k":"\u12"})", "k", s));
  CHECK(jsonQuote("a\"b\\c\n\x01" "\xC3\xA9") == "\"a\\\"b\\\\c\\n\\u0001\xC3\xA9\"");
  std::string back;
  REQUIRE(jsonDecodeString(jsonQuote("C:\\dir\\file \"1\".vpoeta"), back));
  CHECK(back == "C:\\dir\\file \"1\".vpoeta");
  bool b = false;
  REQUIRE(jsonGetBool(R"({"ok":true})", "ok", b));
  CHECK(b);
  CHECK_FALSE(jsonGetInt(R"({"id":1.5})", "id", id));
  REQUIRE(jsonGetInt(R"({"id":-1})", "id", id));
  CHECK(id == -1);
  CHECK(jsonStringArray({"a", "C:\\b"}) == R"(["a","C:\\b"])");
  CHECK(jsonStringArray({}) == "[]");
}

TEST_CASE("shell: json arrays") {
  std::vector<std::string_view> items;
  REQUIRE(jsonArrayItems(R"([1, "a,b", {"x":[2,3]}, [] ])", items, 10));
  REQUIRE(items.size() == 4);
  CHECK(items[0] == "1");
  CHECK(items[1] == "\"a,b\"");
  CHECK(items[2] == R"({"x":[2,3]})");
  CHECK(items[3] == "[]");
  REQUIRE(jsonArrayItems(" [ ] ", items, 10));
  CHECK(items.empty());
  CHECK_FALSE(jsonArrayItems("[1,2,3]", items, 2));
  CHECK_FALSE(jsonArrayItems("[1,2", items, 10));
  CHECK_FALSE(jsonArrayItems("[1,,2]", items, 10));
  CHECK_FALSE(jsonArrayItems("{}", items, 10));
}

TEST_CASE("shell: requests from the page are parsed and routed") {
  WebRequest r;
  REQUIRE(parseWebRequest(R"({"id":7,"cmd":"dialog.openFile","params":{"multiple":true}})", r));
  CHECK(r.id == 7);
  CHECK(r.cmd == "dialog.openFile");
  CHECK(r.params == R"({"multiple":true})");
  CHECK(routeCommand(r.cmd) == Route::Shell);
  REQUIRE(parseWebRequest(R"({"cmd":"cue.page","id":3})", r));
  CHECK(r.params == "{}");
  REQUIRE(parseWebRequest(R"({"id":4,"cmd":"engine.hello","params":null})", r));
  CHECK(r.params == "{}");
  CHECK(routeCommand("dialog.saveFile") == Route::Shell);
  CHECK(routeCommand("dialog.droppedFiles") == Route::Shell);
  CHECK(routeCommand("shell.revealFile") == Route::Shell);
  CHECK(routeCommand("shell.openExternal") == Route::Shell);
  CHECK(routeCommand("shell.runProgram") == Route::Reject);
  CHECK(routeCommand("dialog.anything") == Route::Reject);
  CHECK(routeCommand("engine.ping") == Route::Engine);
  CHECK(routeCommand("translate.start") == Route::Engine);
  CHECK(routeCommand("settings.set") == Route::Engine);
  CHECK_FALSE(parseWebRequest(R"({"id":"x","cmd":"a"})", r));
  CHECK_FALSE(parseWebRequest(R"({"id":1})", r));
  CHECK_FALSE(parseWebRequest(R"({"id":1,"cmd":""})", r));
  CHECK_FALSE(parseWebRequest(R"({"id":1,"cmd":"a","params":[1]})", r));
  CHECK_FALSE(parseWebRequest("[1,2]", r));
}

TEST_CASE("shell: engine lines are responses or events") {
  EngineLine e = classifyEngineLine(R"({"id":4,"ok":true,"result":{"total":3,"cues":[]}})");
  CHECK(e.kind == EngineLine::Response);
  CHECK(e.id == 4);
  CHECK(e.ok);
  e = classifyEngineLine(R"({"id":-1,"ok":false,"error":{"code":"internal"}})");
  CHECK(e.kind == EngineLine::Response);
  CHECK(e.id == kPingId);
  CHECK_FALSE(e.ok);
  e = classifyEngineLine(R"({"id":null,"ok":false,"error":{"code":"bad_params"}})");
  CHECK(e.kind == EngineLine::Response);
  CHECK(e.id == 0);
  e = classifyEngineLine(R"({"event":"translate.progress","jobId":1,"done":3})");
  CHECK(e.kind == EngineLine::Event);
  CHECK(e.event == "translate.progress");
  CHECK(classifyEngineLine("hello").kind == EngineLine::Invalid);
  CHECK(classifyEngineLine(R"({"event":""})").kind == EngineLine::Invalid);
}

TEST_CASE("shell: message builders produce parseable JSON") {
  std::string ok = makeOkResponse(5, R"({"path":null,"paths":[]})");
  EngineLine e = classifyEngineLine(ok);
  CHECK(e.kind == EngineLine::Response);
  CHECK(e.ok);
  std::string err = makeErrorResponse(6, "bad_params", "bad", "Say \"hi\"");
  e = classifyEngineLine(err);
  CHECK(e.id == 6);
  CHECK_FALSE(e.ok);
  std::string_view errObj;
  REQUIRE(jsonFindRaw(err, "error", errObj));
  std::string hint;
  REQUIRE(jsonGetString(errObj, "hint", hint));
  CHECK(hint == "Say \"hi\"");
  CHECK(makeEvent("power.status", "\"onBattery\":true") == R"({"event":"power.status","onBattery":true})");
  CHECK(makeEvent("x", "") == R"({"event":"x"})");
  CHECK(makeRequest(kPingId, "engine.ping", "") == R"({"id":-1,"cmd":"engine.ping","params":{}})");
}

TEST_CASE("shell: watchdog pings every 2 s and restarts after 10 s of silence") {
  Watchdog w;  // 2,000 ms interval, 10,000 ms timeout
  w.reset(1000);
  CHECK(w.tick(1500) == Watchdog::Action::None);
  CHECK(w.tick(3000) == Watchdog::Action::SendPing);
  CHECK(w.tick(3500) == Watchdog::Action::None);
  w.onPong(3010);
  CHECK(w.tick(5000) == Watchdog::Action::SendPing);
  CHECK(w.tick(7000) == Watchdog::Action::SendPing);
  CHECK(w.tick(9000) == Watchdog::Action::SendPing);
  CHECK(w.tick(11000) == Watchdog::Action::SendPing);
  CHECK(w.tick(13009) == Watchdog::Action::SendPing);  // silent 9,999 ms
  CHECK(w.silentForMs(13009) == 9999);
  CHECK(w.tick(13010) == Watchdog::Action::Restart);   // silent 10,000 ms
  CHECK(w.tick(13011) == Watchdog::Action::None);      // reset by the restart
  w.onPong(14000);
  CHECK(w.tick(15011) == Watchdog::Action::SendPing);
  // A pong every 2 s keeps it alive forever.
  Watchdog alive;
  alive.reset(0);
  for (int64_t t = 0; t < 120000; t += 500) {
    CHECK(alive.tick(t) != Watchdog::Action::Restart);
    if (t % 2000 == 0) { alive.onPong(t + 30); }
  }
}

TEST_CASE("shell: at most 3 restarts in 60 s") {
  RestartLimiter lim;
  CHECK(lim.allow(0));
  CHECK(lim.allow(10000));
  CHECK(lim.allow(20000));
  CHECK_FALSE(lim.allow(30000));
  CHECK_FALSE(lim.allow(59999));
  CHECK(lim.allow(60000));
  CHECK_FALSE(lim.allow(60001));
  CHECK(lim.allow(80000));
  lim.clear();
  CHECK(lim.allow(80001));
}

TEST_CASE("shell: session tracker follows the project, settings and requests in flight") {
  SessionTracker t;
  CHECK_FALSE(t.busy());
  CHECK_FALSE(t.hasProject());
  t.onRequest(1, "project.open", R"({"path":"C:\\a\\b.vpoeta"})");
  t.onRequest(2, "cue.page", R"({"from":0,"count":200})");
  CHECK(t.inFlight() == 2);
  t.onResponse(1, true,
               R"({"id":1,"ok":true,"result":{"project":{"path":"C:\\a\\b.vpoeta","autosavePath":"C:\\a\\b.vpoeta.autosave"},"warnings":[]}})");
  CHECK(t.recoverBase() == "C:\\a\\b.vpoeta");
  CHECK(t.projectPath() == "C:\\a\\b.vpoeta");
  t.onResponse(2, true, R"({"id":2,"ok":true,"result":{"total":0,"cues":[]}})");
  CHECK_FALSE(t.busy());

  // A new untitled project: recover from its autosave base under <data>/unsaved.
  t.onRequest(3, "project.new", R"({"kind":"subs","pair":"en-la","sourcePath":"C:\\x.srt"})");
  t.onResponse(3, true,
               R"({"id":3,"ok":true,"result":{"project":{"path":null,"autosavePath":"C:\\d\\unsaved\\untitled-1.vpoeta.autosave"}}})");
  CHECK(t.recoverBase() == "C:\\d\\unsaved\\untitled-1.vpoeta");
  CHECK(t.projectPath().empty());
  CHECK(t.hasProject());

  // Failed save keeps the old state; a good save names the file.
  t.onRequest(4, "project.saveAs", R"({"path":"D:\\new.vpoeta"})");
  t.onResponse(4, false, R"({"id":4,"ok":false,"error":{"code":"io"}})");
  CHECK(t.projectPath().empty());
  t.onRequest(5, "project.saveAs", R"({"path":"D:\\new.vpoeta"})");
  t.onResponse(5, true, R"({"id":5,"ok":true,"result":{"path":"D:\\new.vpoeta","at":"2026-10-06T10:00:00Z"}})");
  CHECK(t.projectPath() == "D:\\new.vpoeta");
  CHECK(t.recoverBase() == "D:\\new.vpoeta");

  t.onRequest(6, "project.close", "{}");
  t.onResponse(6, true, R"({"id":6,"ok":true,"result":{"path":"D:\\new.vpoeta","at":"x"}})");
  CHECK_FALSE(t.hasProject());

  t.onRequest(7, "settings.set", R"({"patch":{"lang":"es-MX","theme":"dark"}})");
  t.onResponse(7, true, R"({"id":7,"ok":true,"result":{"lang":"es-MX","theme":"dark","export":{"lang":"x"}}})");
  CHECK(t.lang() == "es-MX");
  CHECK(t.theme() == "dark");
  t.noteSettings(R"({"theme":"auto"})");
  CHECK(t.theme() == "auto");
  CHECK(t.lang() == "es-MX");

  t.onRequest(8, "translate.start", "{}");
  t.onRequest(9, "settings.set", R"({"patch":{"theme":"light"}})");
  t.onRequest(10, "settings.set", R"({"patch":"bad"})");
  std::vector<std::string> replay;
  std::vector<long long> lost = t.onEngineRestart(&replay);
  CHECK(lost == std::vector<long long>{8, 9, 10});
  REQUIRE(replay.size() == 1);
  CHECK(replay[0] == R"({"theme":"light"})");
  CHECK_FALSE(t.busy());
  t.onResponse(99, true, "{}");  // unknown ids are ignored
}

TEST_CASE("shell: in-flight list is bounded") {
  SessionTracker t;
  for (long long i = 0; i < long(SessionTracker::kMaxInFlight) + 100; ++i) { t.onRequest(i + 1, "x", "{}"); }
  CHECK(t.inFlight() == SessionTracker::kMaxInFlight);
}

TEST_CASE("shell: restart sequence replays settings and recovers the project") {
  SessionTracker t;
  t.setProject("C:\\p\\film.vpoeta", "C:\\p\\film.vpoeta");
  RestartSequence seq;
  RestartSequence::Step s = seq.start(t, {R"({"theme":"dark"})"}, 0);
  REQUIRE(s.send.size() == 3);
  CHECK(cmdOf(s.send[0]) == "settings.set");
  CHECK(paramsOf(s.send[0]) == R"({"patch":{"theme":"dark"}})");
  CHECK(cmdOf(s.send[1]) == "settings.get");
  CHECK(cmdOf(s.send[2]) == "project.recover");
  CHECK(paramsOf(s.send[2]) == R"({"path":"C:\\p\\film.vpoeta"})");
  CHECK(s.event.empty());
  CHECK(seq.active());
  CHECK(seq.onResponse(kSettingsId, true, 10).event.empty());
  s = seq.onResponse(kRecoverId, true, 20);
  CHECK(s.event == R"({"event":"engine.restarted","recovered":true,"reopened":false,"path":"C:\\p\\film.vpoeta"})");
  CHECK_FALSE(seq.active());
  CHECK(seq.onResponse(kRecoverId, true, 30).event.empty());  // over: late answers are ignored
}

TEST_CASE("shell: restart sequence falls back to project.open, then gives up") {
  SessionTracker t;
  t.setProject("C:\\p\\film.vpoeta", "C:\\p\\film.vpoeta");
  RestartSequence seq;
  seq.start(t, {}, 0);
  RestartSequence::Step s = seq.onResponse(kRecoverId, false, 10);
  REQUIRE(s.send.size() == 1);
  CHECK(cmdOf(s.send[0]) == "project.open");
  s = seq.onResponse(kReopenId, true, 20);
  CHECK(s.event == R"({"event":"engine.restarted","recovered":false,"reopened":true,"path":"C:\\p\\film.vpoeta"})");

  seq.start(t, {}, 100);
  seq.onResponse(kRecoverId, false, 110);
  s = seq.onResponse(kReopenId, false, 120);
  CHECK(s.event == R"({"event":"engine.restarted","recovered":false,"reopened":false,"path":null})");

  // Untitled project without autosave: nothing to reopen.
  t.setProject("C:\\d\\unsaved\\untitled-1.vpoeta", "");
  seq.start(t, {}, 200);
  s = seq.onResponse(kRecoverId, false, 210);
  CHECK(s.send.empty());
  CHECK(s.event == R"({"event":"engine.restarted","recovered":false,"reopened":false,"path":null})");

  // No project open: the event comes at once, after the settings lines.
  SessionTracker empty;
  s = seq.start(empty, {}, 300);
  REQUIRE(s.send.size() == 1);
  CHECK(cmdOf(s.send[0]) == "settings.get");
  CHECK(s.event == R"({"event":"engine.restarted","recovered":false,"reopened":false,"path":null})");
  CHECK_FALSE(seq.active());

  // Timeout of a recovery that never answers.
  seq.start(t, {}, 1000);
  CHECK_FALSE(seq.timedOut(1000 + RestartSequence::kTimeoutMs));
  CHECK(seq.timedOut(1001 + RestartSequence::kTimeoutMs));
  seq.cancel();
  CHECK_FALSE(seq.timedOut(1001 + RestartSequence::kTimeoutMs));
}

TEST_CASE("shell: log rotation at 2 MB") {
  CHECK(kLogCapBytes == 2u * 1024 * 1024);
  CHECK_FALSE(logNeedsRotation(0, 10 * kLogCapBytes));
  CHECK_FALSE(logNeedsRotation(kLogCapBytes - 10, 10));
  CHECK(logNeedsRotation(kLogCapBytes - 10, 11));
  auto plan = logRotationPlan(std::string("engine.log"), 2);
  REQUIRE(plan.size() == 3);
  CHECK(plan[0] == std::make_pair(std::string("engine.log.2"), std::string()));
  CHECK(plan[1] == std::make_pair(std::string("engine.log.1"), std::string("engine.log.2")));
  CHECK(plan[2] == std::make_pair(std::string("engine.log"), std::string("engine.log.1")));
  auto wplan = logRotationPlan(std::wstring(L"e.log"), 1);
  REQUIRE(wplan.size() == 2);
  CHECK(wplan[0].first == L"e.log.1");
  CHECK(wplan[1].second == L"e.log.1");
}

TEST_CASE("shell: window state round trip, sanitising and DPI sizing") {
  Placement p;
  p.x = -1200;
  p.y = 40;
  p.w = 1440;
  p.h = 900;
  p.maximized = true;
  std::string json = placementToJson(p);
  CHECK(json.find("\"window.maximized\":true") != std::string::npos);
  Placement q;
  REQUIRE(placementFromJson(json, q));
  CHECK(q.x == p.x);
  CHECK(q.y == p.y);
  CHECK(q.w == p.w);
  CHECK(q.h == p.h);
  CHECK(q.maximized);
  CHECK_FALSE(placementFromJson(R"({"window.x":1,"window.y":2,"window.w":0,"window.h":5})", q));
  CHECK_FALSE(placementFromJson(R"({"x":1,"y":2,"w":10,"h":5})", q));
  CHECK_FALSE(placementFromJson("garbage", q));

  Rect work{0, 0, 1920, 1040};
  Placement d = defaultPlacement(work, 1440, 900);
  CHECK(d.x == 240);
  CHECK(d.y == 70);
  Placement off;
  off.x = 5000;
  off.y = -300;
  off.w = 800;
  off.h = 500;
  Placement s = sanitizePlacement(off, work, 1024, 640);
  CHECK(s.w == 1024);
  CHECK(s.h == 640);
  CHECK(s.x == 1920 - 1024);
  CHECK(s.y == 0);

  CHECK(dipToPx(1024, 96) == 1024);
  CHECK(dipToPx(1024, 120) == 1280);
  CHECK(dipToPx(640, 144) == 960);
  CHECK(dipToPx(3, 120) == 4);
  CHECK(dipToPx(100, 0) == 100);
  Size frame{20, 49};
  Size min125 = outerSizeForClient(Size{1024, 640}, 120, frame, Size{1920, 1020});
  CHECK(min125.w == 1280 + 20);
  CHECK(min125.h == 800 + 49);
  Size clamp = outerSizeForClient(Size{1024, 640}, 192, frame, Size{1920, 1020});
  CHECK(clamp.w == 1920);
  CHECK(clamp.h == 1020);
}

TEST_CASE("shell: app origin and external URLs") {
  CHECK(isAppUri("https://app.vetuspoeta/index.html"));
  CHECK(isAppUri("HTTPS://APP.VETUSPOETA/js/vp_app.js"));
  CHECK_FALSE(isAppUri("http://app.vetuspoeta/index.html"));
  CHECK_FALSE(isAppUri("https://app.vetuspoeta.evil/index.html"));
  CHECK(isSafeExternalUrl("https://en.wiktionary.org/wiki/puella"));
  CHECK(isSafeExternalUrl("http://example.com"));
  CHECK_FALSE(isSafeExternalUrl("file:///C:/Windows/system32/calc.exe"));
  CHECK_FALSE(isSafeExternalUrl("javascript:alert(1)"));
  CHECK_FALSE(isSafeExternalUrl("ms-settings:"));
  CHECK_FALSE(isSafeExternalUrl("https://"));
  CHECK_FALSE(isSafeExternalUrl("https:///x"));
  CHECK_FALSE(isSafeExternalUrl("https://a b"));
  CHECK_FALSE(isSafeExternalUrl("https://a\"b"));
  CHECK_FALSE(isSafeExternalUrl("https://a|b"));
  CHECK_FALSE(isSafeExternalUrl("C:\\x.exe"));
  CHECK_FALSE(isSafeExternalUrl("https://" + std::string(3000, 'a')));
}

TEST_CASE("shell: paths for shell.revealFile") {
  std::string out;
  REQUIRE(normaliseRevealPath("C:\\Users\\Ana\\Docs\\film.srt", out));
  CHECK(out == "C:\\Users\\Ana\\Docs\\film.srt");
  REQUIRE(normaliseRevealPath("d:/a/b c/\xC3\x81lbum.vpoeta", out));
  CHECK(out == "d:\\a\\b c\\\xC3\x81lbum.vpoeta");
  REQUIRE(normaliseRevealPath("C:\\Users\\", out));
  REQUIRE(normaliseRevealPath("\\\\server\\share\\x.srt", out));
  REQUIRE(normaliseRevealPath("\\\\server\\share", out));
  CHECK_FALSE(normaliseRevealPath("relative\\x.srt", out));
  CHECK_FALSE(normaliseRevealPath("C:x.srt", out));
  CHECK_FALSE(normaliseRevealPath("\\x.srt", out));
  CHECK_FALSE(normaliseRevealPath("\\\\?\\C:\\x.srt", out));
  CHECK_FALSE(normaliseRevealPath("\\\\server", out));
  CHECK_FALSE(normaliseRevealPath("\\\\\\share\\x", out));
  CHECK_FALSE(normaliseRevealPath("C:\\a\\..\\Windows\\x", out));
  CHECK_FALSE(normaliseRevealPath("C:\\a\\.\\x", out));
  CHECK_FALSE(normaliseRevealPath("C:\\a\\\\x", out));
  CHECK_FALSE(normaliseRevealPath("C:\\a\\x.srt:stream", out));
  CHECK_FALSE(normaliseRevealPath("C:\\a\\*.srt", out));
  CHECK_FALSE(normaliseRevealPath("C:\\a\\x\".srt", out));
  CHECK_FALSE(normaliseRevealPath(std::string("C:\\a\nb"), out));
  CHECK_FALSE(normaliseRevealPath("C:\\\xC3", out));  // broken UTF-8
  CHECK_FALSE(normaliseRevealPath("", out));
  CHECK(out.empty());
}

TEST_CASE("shell: file URIs of dropped files become paths") {
  std::string p;
  REQUIRE(filePathFromFileUri("file:///C:/Users/Ana/My%20Films/film.srt", p));
  CHECK(p == "C:\\Users\\Ana\\My Films\\film.srt");
  REQUIRE(filePathFromFileUri("file:///c:/x/%C3%A9t%C3%A9.vtt", p));
  CHECK(p == "c:\\x\\\xC3\xA9t\xC3\xA9.vtt");
  REQUIRE(filePathFromFileUri("FILE://server/share/a.ass", p));
  CHECK(p == "\\\\server\\share\\a.ass");
  REQUIRE(filePathFromFileUri("file://localhost/C:/a.txt", p));
  CHECK(p == "C:\\a.txt");
  CHECK_FALSE(filePathFromFileUri("file:///C:/a/../b.srt", p));
  CHECK_FALSE(filePathFromFileUri("file:///C:/a%2", p));
  CHECK_FALSE(filePathFromFileUri("file:///etc/passwd", p));
  CHECK_FALSE(filePathFromFileUri("https://x/y", p));
}

TEST_CASE("shell: extensions, command line and argument quoting") {
  CHECK(extensionOf("C:\\a.b\\file.SRT") == "srt");
  CHECK(extensionOf("C:\\a.b\\file") == "");
  CHECK(extensionOf("file.") == "");
  CHECK(isOpenableFile("x.vpoeta"));
  CHECK(isOpenableFile("x.VTT"));
  CHECK(isOpenableFile("x.ssa"));
  CHECK_FALSE(isOpenableFile("x.exe"));
  CHECK_FALSE(isOpenableFile("x.srt.lnk"));
  CommandLine c = parseCommandLine({"--devtools", "C:\\Users\\Ana\\\xC3\x81lbum.VPOETA", "--other", "film.srt", "x.exe"});
  CHECK(c.devtools);
  REQUIRE(c.files.size() == 2);
  CHECK(c.files[0] == "C:\\Users\\Ana\\\xC3\x81lbum.VPOETA");
  CHECK(c.files[1] == "film.srt");
  c = parseCommandLine({"photo.jpg"});
  CHECK_FALSE(c.devtools);
  CHECK(c.files.empty());
  std::vector<std::string> many(40, "a.srt");
  CHECK(parseCommandLine(many).files.size() == kMaxForwardPaths);

  std::string dir, name;
  splitDirName("C:\\Users\\x\\out.srt", dir, name);
  CHECK(dir == "C:\\Users\\x");
  CHECK(name == "out.srt");
  splitDirName("C:\\out.srt", dir, name);
  CHECK(dir == "C:\\");
  splitDirName("out.srt", dir, name);
  CHECK(dir.empty());
  CHECK(name == "out.srt");

  CHECK(quoteWindowsArg("serve") == "serve");
  CHECK(quoteWindowsArg("") == "\"\"");
  CHECK(quoteWindowsArg("C:\\Program Files\\vetus poeta\\data") == "\"C:\\Program Files\\vetus poeta\\data\"");
  CHECK(quoteWindowsArg("C:\\a b\\") == "\"C:\\a b\\\\\"");  // trailing backslash doubled before the quote
  CHECK(quoteWindowsArg("a\"b") == "\"a\\\"b\"");
  CHECK(quoteWindowsArg("a\\\"b c") == "\"a\\\\\\\"b c\"");
  CHECK(quoteWindowsArg("C:\\nospace\\") == "C:\\nospace\\");
}

TEST_CASE("shell: file dialog filters") {
  std::vector<FileFilter> f;
  REQUIRE(parseFilters(R"([{"name":"Subt\u00edtulos","extensions":["srt","VTT",".ass"]},{"name":"Todo","patterns":["*.*"]}])", f));
  REQUIRE(f.size() == 2);
  CHECK(f[0].name == "Subt\xC3\xADtulos");
  CHECK(f[0].pattern == "*.srt;*.vtt;*.ass");
  CHECK(f[0].firstExt == "srt");
  CHECK(f[1].pattern == "*.*");
  CHECK(f[1].firstExt.empty());
  REQUIRE(parseFilters(R"([{"name":"Projects","patterns":["*.vpoeta"]}])", f));
  CHECK(f[0].pattern == "*.vpoeta");
  REQUIRE(parseFilters("[]", f));
  CHECK(f.empty());
  // The start screen's form: a plain list of extensions -> one unnamed filter.
  REQUIRE(parseFilters(R"(["srt","vtt","ass","ssa","txt","vpoeta"])", f));
  REQUIRE(f.size() == 1);
  CHECK(f[0].name.empty());
  CHECK(f[0].pattern == "*.srt;*.vtt;*.ass;*.ssa;*.txt;*.vpoeta");
  CHECK(f[0].firstExt == "srt");
  CHECK_FALSE(parseFilters(R"(["srt",{"name":"x"}])", f));
  CHECK_FALSE(parseFilters(R"(["s rt"])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"x","extensions":["s;rt"]}])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"x","extensions":["../x"]}])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"x","extensions":[]}])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"","extensions":["srt"]}])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"x\n","extensions":["srt"]}])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"x","patterns":["srt"]}])", f));
  CHECK_FALSE(parseFilters(R"([{"name":"x"}])", f));
  CHECK_FALSE(parseFilters(R"({"name":"x"})", f));
  std::string many = "[";
  for (int i = 0; i < 17; ++i) { many += std::string(i ? "," : "") + R"({"name":"x","extensions":["srt"]})"; }
  CHECK_FALSE(parseFilters(many + "]", f));
}

TEST_CASE("shell: WM_COPYDATA payload from a second instance") {
  std::vector<std::string> in{"C:\\a\\film.srt", "D:\\\xC3\x81lbum\\p.vpoeta"};
  std::string payload = encodeOpenPayload(in);
  CHECK(payload == "C:\\a\\film.srt\nD:\\\xC3\x81lbum\\p.vpoeta");
  std::vector<std::string> out;
  REQUIRE(decodeOpenPayload(kCopyDataMagic, payload.data(), payload.size(), out));
  CHECK(out == in);
  std::string withNul = payload + '\0';
  REQUIRE(decodeOpenPayload(kCopyDataMagic, withNul.data(), withNul.size(), out));
  CHECK(out == in);
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic + 1, payload.data(), payload.size(), out));
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, payload.data(), 0, out));
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, nullptr, 4, out));
  std::string bad = "C:\\a\xFF.srt";
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, bad.data(), bad.size(), out));
  std::string empties = "C:\\a.srt\n\nC:\\b.srt";
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, empties.data(), empties.size(), out));
  std::string ctrl = "C:\\a\tb.srt";
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, ctrl.data(), ctrl.size(), out));
  std::string big(kMaxCopyDataBytes + 1, 'a');
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, big.data(), big.size(), out));
  std::string tooMany;
  for (size_t i = 0; i <= kMaxForwardPaths; ++i) { tooMany += (i ? "\n" : "") + std::string("C:\\x.srt"); }
  CHECK_FALSE(decodeOpenPayload(kCopyDataMagic, tooMany.data(), tooMany.size(), out));
  CHECK(out.empty());
  CHECK(encodeOpenPayload({"", "a\nb", "C:\\ok.srt"}) == "C:\\ok.srt");
}

TEST_CASE("shell: UTF-8 validation") {
  CHECK(isValidUtf8(""));
  CHECK(isValidUtf8("abc \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
  CHECK_FALSE(isValidUtf8("\xC0\xAF"));          // overlong
  CHECK_FALSE(isValidUtf8("\xE0\x80\xAF"));      // overlong
  CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));      // surrogate
  CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80"));  // > U+10FFFF
  CHECK_FALSE(isValidUtf8("\xC3"));              // truncated
  CHECK_FALSE(isValidUtf8("\xE2\x82"));
  CHECK_FALSE(isValidUtf8("\x80"));
}

TEST_CASE("shell: theme and language") {
  CHECK(themeIsDark("dark", false));
  CHECK_FALSE(themeIsDark("light", true));
  CHECK(themeIsDark("auto", true));
  CHECK_FALSE(themeIsDark("auto", false));
  CHECK(themeIsDark("", true));
  CHECK(uiLanguage("es-MX") == "es-MX");
  CHECK(uiLanguage("ES") == "es-MX");
  CHECK(uiLanguage("en-US") == "en-US");
  CHECK(uiLanguage("") == "en-US");
  CHECK(uiLanguage("fr") == "en-US");
}

TEST_CASE("shell: parsers survive every truncation of a message") {
  const std::string msg =
      R"({"id":12,"cmd":"cue.set","params":{"index":3,"text":"a\"b\\c\u00e9","filters":[{"name":"x","extensions":["srt"]}],)"
      R"("project":{"path":"C:\\p.vpoeta","autosavePath":"C:\\p.vpoeta.autosave"}},"ok":true,"event":"x","window.x":1})";
  for (size_t n = 0; n <= msg.size(); ++n) {
    std::string cut = msg.substr(0, n);  // own buffer so ASan sees reads past the end
    WebRequest r;
    parseWebRequest(cut, r);
    classifyEngineLine(cut);
    std::string s;
    jsonGetString(cut, "cmd", s);
    SessionTracker t;
    t.onRequest(1, "project.open", cut);
    t.onResponse(1, true, cut);
    t.onRequest(2, "settings.set", cut);
    t.onResponse(2, true, cut);
    Placement p;
    placementFromJson(cut, p);
    std::vector<FileFilter> f;
    parseFilters(cut, f);
    std::vector<std::string_view> items;
    jsonArrayItems(cut, items, 100);
    filePathFromFileUri(cut, s);
    normaliseRevealPath(cut, s);
    std::vector<std::string> paths;
    decodeOpenPayload(kCopyDataMagic, cut.data(), cut.size(), paths);
  }
  CHECK(true);
}
