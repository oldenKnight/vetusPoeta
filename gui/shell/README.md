# gui/shell (`VetusPoeta.exe`)

Win32 + WebView2 host, ported from the prototype shell. Windows only (`VP_BUILD_GUI=ON`, MSVC or MinGW-w64);
its portable logic `vp_host_logic` (`host_logic.h`, no Windows headers) builds everywhere and is tested in
`vp_tests` (`engine/tests/test_shell_logic.cpp`, `vp_tests -tc='shell*'`). Build and dist: docs/BUILD.md.

## Files
`app.cpp` window, WebView2, message routing, shell commands, supervision; `engine_host.*` vpengine process
(pipes, job object, reader/writer threads, stderr log); `shell_util.*` RAII handles/COM, folders, strings;
`host_logic.*` framing, routing, watchdog, restart sequence, paths, WM_COPYDATA, window state;
`webview2_api.h` picks `WebView2.h` (NuGet, downloaded at configure) or `webview2_subset.h` (checked in, made by
`tools/webview2_subset.py gen|verify <WebView2.h>`); `VetusPoeta.manifest/.rc.in`; `tools/make_placeholder_icon.py`
writes `assets/icon.ico`.

## Runtime layout and folders
`<exe>\ui\` is served at `https://app.vetuspoeta/index.html` (virtual host mapping, access DENY for other
origins; CSP is the one in index.html). `<exe>\vpengine.exe serve --data <data> [--lexicons <exe>\data]`.
`<data>` = `%VP_DATA_DIR%` or `%LOCALAPPDATA%\vetus-poeta`: `settings.json` (engine), `window.json`
(`window.x/y/w/h/maximized`), `logs\engine.log` (engine stderr + shell notes, rotated at 2 MB, 2 kept),
`WebView2\` (browser profile). `VetusPoeta.exe --devtools` enables DevTools (F12), context menus, browser keys.

## Messages (DESIGN 9)
The UI posts JSON strings with `chrome.webview.postMessage`; the shell posts back with `PostWebMessageAsString`
(`e.data` is the JSON text). Requests go to vpengine unchanged except these (any other `dialog.*`/`shell.*`
is refused with `bad_params`):
- `dialog.openFile {filters?, multiple?, title?}` -> `{path|null, paths:[...], cancelled}`
- `dialog.saveFile {suggested|suggestedName, filters?, title?}` -> `{path|null, cancelled}`
- `filters`: `[{"name":"Subtitles","extensions":["srt","vtt"]}]` (or `"patterns":["*.srt"]`), or a plain
  `["srt","vtt"]` (the shell names it and adds "All files"); default = every openable file type.
- `shell.revealFile {path}` (absolute path, Explorer selects it; `not_found` if gone) -> `{}`
- `shell.openExternal {url}` (http/https only, default browser) -> `{}`
- `dialog.droppedFiles {}` posted with `chrome.webview.postMessageWithAdditionalObjects(text, files)` -> `{paths}`
Events from the shell: `dialog.droppedFiles {paths, source:"drop"|"launch"}`, `power.status {onBattery}` (first
page message, then on change), `engine.restarted {recovered, reopened, path|null}`. Drops: a host script
injected before the page (`kDropScript` in app.cpp, ES5, no globals) posts the dropped File objects in the
bubble phase on window, after the page's handlers, and cancels the browser default, so the page gets the
event with real paths even when it cancelled the drop itself; a file URL navigation is caught as a fallback.
"launch" = files on the command line or forwarded by a second instance (WM_COPYDATA, single instance mutex).

## Supervision
Ping every 2 s, restart after 10 s without any engine output or when the process exits; requests in flight
get `internal` "engine restarted"; then `settings.set` replays patches that were in flight, `settings.get`,
`project.recover {path:<project or untitled base>}` (fallback `project.open`), then `engine.restarted`.
At most 3 restarts in 60 s, then a native message with the log path. While recovering, requests get `busy`.

## Strings
Native dialogs and hints are looked up in `ui\i18n\<lang>.json` under `app.shell.*` (keys and en-US/es-MX
fallback texts in `shell_util.cpp`), so the string tables can own them; the built-in texts cover a missing ui.

## Tests
Linux: `ctest --test-dir build` (vp_tests). Windows smoke test: the checklist in docs/BUILD.md.
