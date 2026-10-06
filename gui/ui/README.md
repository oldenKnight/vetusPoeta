# gui/ui - the vetus poeta interface (ES5, WebView2)

Contract: DESIGN 13 (files, globals, load order, CSP), PREDESIGN 2 (look), 4.3-4.4 (keys,
undo), 5 (strings), 6 (accessibility, budgets). ES5 only, one IIFE and one `VP_<Name>` global
per file, every visible string in `i18n/en-US.json` and `i18n/es-MX.json`.

## Load order (index.html; enforced by tools/jstest)
css: `tokens.css` (colour tokens light/dark, font stack, Gentium Plus faces), `base.css`,
`components.css`, `screens.css`.
js: `polyfill_promise` (window.Promise only if missing), `vp_dom` (el, on/off registry,
delegate), `vp_timers` (owned timers), `vp_i18n`, `vp_bridge` (WebView2 or mock; events
batched, at most 10 UI updates/s), `vp_mock_engine` (dev only), `vp_store` (cue window capped
at 50,000), `vp_history` (500 entries / 5 MB), `vp_keys` (PREDESIGN 4.3 table), `vp_toast`,
`vp_dialog`, `vp_tour`, `vp_router`, then the screen files as they arrive (`vp_start` ...
`vp_about`, DESIGN 13 order), `vp_debug`, `vp_app` (boots when `#app[data-autoboot]`).
Until `vp_start.js` exists, `vp_app.js` mounts a placeholder start screen (engine status
line, EN/ES and theme switches, font sample, every shared component).

## Run it in a browser (mock engine)
    node gui/ui/dev/serve.js          # then open the printed URL:
    http://127.0.0.1:8123/index.html?mock=1&debug=1
A plain `file://` open runs the mock too, but browsers block the JSON string tables and
the fonts there, so you would see keys instead of text. Query flags: `mock=1` (practice
engine), `debug=1` (VP_Debug.stats() and router leak checks), `lang=en-US|es-MX`,
`theme=auto|light|dark`, `reducedMotion=1`. Inside the Windows app the shell provides
`window.chrome.webview` and the mock is never used.

## Tests
    tools/jstest.sh [pattern] [--verbose]     # Node >= 18, no npm packages
Phases: selftest (planted faults under tools/jstest/selftest must be caught), ES5 lint +
`node --check`, per-file contract (one global with the DESIGN 13 name, IIFE, no listener or
timer at load), index.html (CSP exact, landmarks, script order, no inline code), CSS
(tokens equal PREDESIGN 2.1, WCAG contrast of token pairs, 60 KB budget), i18n (same sorted
keys, no empty values, same `{placeholders}`, every used key exists, es-MX deny list and
opening ¿ ¡), unit tests `gui/ui/tests/*.test.js` against a DOM stub with fake timers and
no native Promise, size report. Dynamic key families: `tools/jstest/dynamic_keys.json`.

    node gui/ui/dev/smoke.js          # Playwright + Chromium; prints SKIP if absent
Boots `?mock=1&debug=1` in Chromium: no console errors or CSP violations, 50 mount/destroy
cycles return VP_Debug.stats() to baseline, Gentium Plus renders the Latin and polytonic
Greek sample (document.fonts.check and Chromium's platform-font report), auto dark mode,
keyboard dialog and tour, DOM <= 800 nodes; screenshots in `dev/out/` (gitignored).

## Budgets (PREDESIGN 6.2), reported by tools/jstest
CSS <= 60 KB; JS <= 300 KB gzip in total; fonts <= 2.6 MB; DOM <= 800 nodes; idle heap
<= 60 MB. Screens: `mount(root, params)` / `destroy()` must return VP_Dom, VP_Timers,
VP_I18n, VP_Store, VP_Bridge, VP_Keys and VP_History counts to their pre-mount values.

## Error codes
The engine's codes (DESIGN 9) map to `error.<code>.title` / `.hint`; the bridge adds two
UI-only codes: `timeout` (no answer in time) and `no_engine` (no transport).
