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
`vp_dialog`, `vp_tour`, `vp_router`, then the screen files as they arrive (`vp_start`,
`vp_workspace`, `vp_cuelist`, `vp_panes` ... `vp_about`, DESIGN 13 order), `vp_debug`,
`vp_app` (boots when `#app[data-autoboot]`). The B3 placeholder screen (engine status line,
EN/ES and theme switches, font sample, every shared component) stays in `vp_app.js` as
`VP_App.placeholder`; the smoke test mounts it for its font checks.

## Screens (B6)
Routing: `VP_App` follows `VP_Store 'project'`: a project routes to `workspace`, `null` back
to `start` (one tick later, and open toasts are dismissed with the screen that showed them).
- `vp_start.js` (`VP_Start`, PREDESIGN 1.1): subtitle-file card (drop zone, "Choose file..."
  through `dialog.openFile`), text card (`project.new {kind:"text"}`), pair picker (Greek
  pairs disabled, "coming later"), "Orbergise a Latin file" (`la-la`), recent projects
  (engine-kept `settings.recentProjects` paths, max 20; display details in the extra settings
  key `recentInfo` {path: {name, pair, kind, cues, translated, needReview, at}}), "Try the
  sample" (`project.new` with `<dataDir>/samples/sample.<src>.srt`), status line, recovery
  banner when `project.open` returns `recoverable` (Recover focused by default /
  Keep the saved version), red card when the Latin dictionary is missing (its path shown).
  Files dropped anywhere on the window (DOM `drop` in a browser, `dialog.droppedFiles` from
  the shell). `normalizeProject()` accepts the engine's project object (`manifest`, `stats`)
  and the mock's flat one.
- `vp_workspace.js` (`VP_Workspace`, PREDESIGN 1.2): top bar (wordmark = close project,
  name, save state, pair, Translate | Orbergise tabs, Undo/Redo, drawer toggle below 1180 px,
  gear, ?), cue list 320 px | centre >= 560 px | right panel host 360 px
  (`VP_Workspace.panelHost()`, filled by B7), status bar (autosave text `aria-live`, counts,
  engines, network). Owns the PREDESIGN 4.3 shortcut handlers while mounted and the engine
  commands `VP_Workspace.cmd.{review, edit, choose, undo, redo, translate, cancel}`: the store
  changes at once, `VP_History` gets before/after snapshots, undo/redo apply them when the
  engine's `changedIndices` match, else the changed pages are fetched again. Save state:
  "Unsaved changes" after a change until the engine's `project.autosaved` event.
- `vp_cuelist.js` (`VP_CueList`): 48 px rows, `windowFor()` = visible rows + 8 + 8, hard cap
  40 rendered rows (recycled pool); data in `VP_Store` from `cue.page` (visible pages first,
  then all pages in the background, 2 in flight); filters All / Needs review / Fix only /
  Edited by me / With emoji / Over reading speed / Unknown names (CueView.flags `cps`,
  `emoji`, `unknownName`); search folds case, macrons, breves, accents and Greek diacritics;
  `role="listbox"` keeps the focus with `aria-activedescendant`; the selected cue stays
  listed under a filter until the selection moves; "Accept all green (n)" with an Undo toast.
  Publishes `VP_Store 'selection' {index}` and `'cueCounts'`.
- `vp_panes.js` (`VP_Panes`): source with rendered markup (italic, ASS override chips),
  timing, duration and cps chip; target word chips (click/Enter/Space -> `VP_Store 'inspect'
  {index, token, text, lang, lemmaId}` for the B7 inspector, plus a small card from
  `word.inspect`); textarea editor over a mirror that underlines unknown words (word.inspect,
  300 ms debounce, LRU 200), Esc / Ctrl+Enter, then "Remember this change? [This phrase]
  [Just this cue]" (`cue.set` again with `remember:"phrase"`); preview strip with the
  Export settings, a 42-character guide and warning chips; up to 3 alternatives (keys 1-3).
  Token offsets from the engine are UTF-8 bytes, so words are placed by text search.

## The mock engine for the screens
`?mock=1` paths: `.../samples/sample.<lang>.srt` = the 12-cue sample (our own sentences);
`...-<N>-cues....vpoeta` = N translated cues (try `VP_Start.openPath('C:\\x\\demo-50000-cues.vpoeta')`
in the console); a name with `recover` or `crash` = recoverable autosave; `missing`,
`damaged` = errors. It keeps `recentProjects`, sends `project.autosaved` 1 s after a change
and flags `cps`, `overflow`, `emoji`, `unknownName`; `VP_MockEngine.options.noLexicon`
drops the Latin dictionary.

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
keyboard dialog and tour, DOM <= 800 nodes. Then the screens: opens the sample, runs a mock
translation to the end (filter "Needs review (n)", first cue to review selected), editor
underline, drawer at 1100 px, closes it (listeners and timers back to the Start baseline),
opens a 50,000-cue project, waits until every cue is paged in, scrolls to the end and back
(rendered cue rows <= 40, DOM nodes <= 800, last cue rendered), times cue selection
(< 100 ms to the next frame), filter and search, closes it (baseline again). About 15 s.
Screenshots in `dev/out/` (gitignored): `start-*` and `workspace-*` in light/dark, en/es.

## Budgets (PREDESIGN 6.2), reported by tools/jstest
CSS <= 60 KB; JS <= 300 KB gzip in total; fonts <= 2.6 MB; DOM <= 800 nodes; idle heap
<= 60 MB. Screens: `mount(root, params)` / `destroy()` must return VP_Dom, VP_Timers,
VP_I18n, VP_Store, VP_Bridge, VP_Keys and VP_History counts to their pre-mount values.

## Error codes
The engine's codes (DESIGN 9) map to `error.<code>.title` / `.hint`; the bridge adds two
UI-only codes: `timeout` (no answer in time) and `no_engine` (no transport).
