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
  through `dialog.openFile`), text card (`project.new {kind:"text"}`), pair picker (from
  `engine.hello.pairs` since B8, see below), "Orbergise a Latin file" (`la-la`), recent projects
  (engine-kept `settings.recentProjects` paths, max 20; display details in the extra settings
  key `recentInfo` {path: {name, pair, kind, cues, translated, needReview, at}}), "Try the
  sample" (`project.new` with the `hello.samples` file of the source language), status line, recovery
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

## Right panel, dialogs and modes (B7)
The workspace's right panel (`VP_Workspace.panelHost()`) carries a tab strip Word | Engines |
Names | Corrections | Words (arrow keys, Home/End; the last tab is kept in the UI-only
settings key `panelTab`); every tab is a module with `mount(el)` / `destroy()` and only the
open one is mounted. The gear opens `VP_Settings`, `?` opens `VP_App.openHelp()` (shortcuts,
tour, about), the Export button and Ctrl+Shift+E open `VP_Export`, the Orbergise mode tab
mounts `VP_Orberg` in the centre. The top bar and the start screen show the SVG lockup
(`img/logo_wordmark*.svg`, one per theme, `VP_App.lockup()`); new icons come from
`img/icons.svg`.
- `vp_inspector.js` (`VP_Inspector`, PREDESIGN 1.2.1, 4.5): follows `VP_Store 'inspect'`
  from the word chips (and the Orbergise panes); headword with macrons, part of speech,
  tier badge (three laurel leaves, filled = level), emoji, gloss in the UI language with
  "(via English)" when `LemmaView.flags` has `gloss-es-pivot`, "This cue uses:" with the
  form in words then abbreviations and a Grammar help link per term (`grammar.<f>.<v>.help`,
  one paragraph each, both languages), "Other forms" (lazy `lemma.get`, paradigm grid with the
  used cell highlighted: cases x number/gender for nominals, person/number x tense per
  mood|voice for verbs, non-finite forms listed), "Why this word?" (W) with the four blocks
  Meaning / Candidates / Form / Evidence from `cue.get` reasons of that token plus the
  checks A1-A9, "Use another word" (candidate form -> `cue.set`, the cue shows Check until
  it is checked again) and "Add to my corrections" (`cue.set` with `remember:"phrase"`). The
  cue's source words are listed as buttons; a source word shows its chosen sense and the
  Latin candidates. Caches: cue.get 20, word.inspect 200, lemma.get 20.
- `vp_engines.js` (`VP_Engines`, 1.2.2): Rules locked on with the lexicons of
  `engine.hello`; local model switch with `model.status` (Install... explains, Find file...
  = `dialog.openFile {filters:["gguf"]}` then `model.locate`); online switch with the
  one-time "what leaves the computer" dialog (UI settings key `onlineExplained`) and Test
  connection (`online.test`); fidelity slider, left = extremely faithful = 3 (any word),
  middle = 2, right = flexible = 1 (basic words, may rephrase), effect line from the
  lexicon's `tiers {t1,t2,t3}` counts when `engine.hello` carries them and the file's share
  from `words.list`; emoji-in-app and macrons-in-file switches; Translate again (selected |
  all). Any engine change calls `VP_Workspace.cmd.markStale()`: translated cues get the grey
  dot in the store (UI state until the next translate.start); edited and reviewed cues are
  never touched.
- `vp_names.js` (`VP_Names`, 4.2): `names.list`, per name count, Keep | Decline | Translate,
  Latin form, gender, declension, Apply -> `names.set` (the engine marks the cues stale; they
  are fetched again), Undo toast sends the previous policy; "Copy as CSV" (there is no
  names import/export command).
- `vp_corrections.js` (`VP_Corrections`): `corrections.list` rows key -> target, scope, count,
  Remove -> `corrections.remove` with Undo (re-created through `cue.set {remember}` on the
  loaded cue with that source; there is no corrections.add).
- `vp_words.js` (`VP_Words`): `words.list` by count with tier badges, filter by tier, share bar
  (`tierShare`), "Copy as list" / "Copy as CSV" (lemma, gloss, tier, count); at most 300 rows
  rendered.
- `vp_orberg.js` (`VP_Orberg`, 1.2.3): Latin file | Original-language file (Choose file... ->
  `dialog.openFile {kind:"original"}` then `orbergise.start {originalPath}`; `cue.get
  .original` afterwards) | Orberg version (changed words underlined with their tier badge,
  from `cue.get` reasons whose `data` has `was`/`now`; a click shows "was -> now" and the
  reason in the Word tab; Edit through `cue.set`); Meaning check chip from `cue.get
  .meaning {percent, missing}`; options tier T1|T2, Keep names, Simplify (UI settings key
  `orberg`), Orbergise all | selected through `VP_Workspace.cmd.orbergise`.
- `vp_export.js` (`VP_Export`, 1.3): format radios (original on), file name with the language
  code, Choose... (`dialog.saveFile`), options from the settings' export defaults (this export
  only), checks (count, fast cues -> Show them, Fix cues -> Review first + the explicit tick),
  3-cue `export.preview`, Export -> `export.write`; io "file exists" -> confirm -> `overwrite:
  true`; success toast with Reveal file (`shell.revealFile`).
- `vp_settings.js` (`VP_Settings`, 1.4): one scrolling dialog with anchors; every control
  applies at once (`VP_Settings.apply(path, value)` -> `VP_App.saveSettings`, nested objects
  merged); `VP_App` follows the store for theme, text size and language; Reset to defaults.
- `vp_about.js` (`VP_About`, 1.5): About, Data sources and licences (each lexicon's `notice`
  verbatim), Software, Fonts (OFL.txt over XHR); links through `shell.openExternal`.
- Tour (`VP_App.startTour`, 1.6): six steps on real controls; the last offers the sample.
  It starts by itself once per `TOUR_VERSION` (mock mode only with `?tour=1`).

## Against the real engine (B8)
The engine's shapes (engine/cli/README.md) are the truth; the mock follows them.
- Boot order: `VP_App` initialises the bridge before the router mounts the first screen, so
  the WebView2 `message` listener belongs to the app and the debug router's leak check is
  clean under the real transport (unit test with a WebView2 stub; `smoke_real.js` checks
  `VP_Debug.failures()`).
- Start screen: the pair picker comes from `engine.hello.pairs` (enabled) and
  `pairsUnavailable [{pair, code, message, hint}]` (disabled; the reason as the option's
  tooltip and, grouped by reason, in a note under the picker: "not available yet" for the
  probe's `bad_params`, else the engine's hint, which names the missing files). Before the
  engine answers, or with an engine without `pairs`, the B6 table applies. The Orbergise
  option follows `la-la`. "Try the sample" opens the `hello.samples` entry of the source
  language (disabled with a tooltip when there is none). The status line names the engine
  (`engineKind` rules | stub, version) and "This version has no local model" for
  `model.reason:"not_built"`. Helpers: `VP_Start.pairInfo(pair)`, `pairReason(info)`,
  `sampleFor(lang)`.
- Warnings: the `translate.start` result's `warnings` codes, the `translate.warning
  {jobId, engine, code, message, hint}` events and any `translate.done` `warnings` fill
  `VP_Store 'warnings'` (one entry per engine, replaced by the next job). A new one shows a
  toast once per job ("Model unavailable: not installed", "Show engines"); the status bar
  carries a chip per engine (engine hint as tooltip) that opens the Engines tab, where the
  warning is listed under its engine with the hint. The Engines tab also says "helps with
  understanding English or Spanish only" when `model.rerankEnabled` is false, "built without
  the local model" for `not_built`, and whether the online check can run (`engines.online`
  and `online.wiktionary`, as the engine's `online.allowed`), with Open Settings.
- Reading pairs (la-en, la-es, grc-en, grc-es; cues flagged `source-tokens`): the source
  pane shows the `cue.get` tokens (the Latin/Greek words) as chips; hover or focus shows a
  card under the text (lemma head, form in words, gloss in the UI language from
  `word.inspect`, else the analysis gloss, tier, emoji, role); click / Enter publishes
  `inspect {side:"analysis"}` and the Word tab shows the entry, the form, the paradigm and
  "Why this reading?" from the `analysis` reason (chosen reading with role and confidence,
  other readings, evidence, checks; no "Use another word"). The target pane shows the
  readable sentence plainly. The interlinear lines (word / lemma / form abbreviations /
  gloss) are a teacher toggle in the source pane head and `Ctrl+I` (settings key
  `interlinear`, off by default). Alternative reasons are looked up as camel-case keys
  ("word by word" -> `target.alt.reason.wordByWord.label`). Export writes the cue targets,
  i.e. the readable text (smoke_real compares the preview with the cue lines).
- Greek: Greek spans carry `lang="grc"` (target, preview, cue list, editor, inspector,
  paradigm); a Greek pair puts `pair-grc` on the workspace root and the app element (Aegean
  accent, PREDESIGN 2.3) and on the Export dialog; the `;` question mark is text as sent.
  The Export dialog always shows polytonic | monotonic, enabled only for a Greek target.
  Paradigm tables for Greek: cases nominative, genitive, dative, accusative, vocative; the
  dual is left out; of several spellings of a cell the Attic (and contracted) ones are shown
  (`extra` of greek.vpl cells; "alternative" last); an empty voice is the middle-passive.
- `cue.set` answers `correctionAdded {id, key, target, scope, count}`: published as
  `VP_Store 'correctionAdded'`, the Corrections tab reloads.
- Names: `names.list` has no detection (only names set through `names.set`, no count): the
  list shows only when the engine returns names; the "Add a name" form (name, what to do,
  Latin form) is always there.
- Orbergise: the tab works only for a Latin project with `la-la` in `hello.pairs`; else it
  says why (not a Latin file / the engine's reason) and sends nothing.

## The mock engine for the screens
`?mock=1` paths: `.../samples/sample.<lang>.srt` = the 12-cue sample (our own sentences);
`...-<N>-cues....vpoeta` = N translated cues (try `VP_Start.openPath('C:\\x\\demo-50000-cues.vpoeta')`
in the console); a name with `recover` or `crash` = recoverable autosave; `missing`,
`damaged` = errors. It keeps `recentProjects`, sends `project.autosaved` 1 s after a change
and flags `cps`, `overflow`, `emoji`, `unknownName`; `VP_MockEngine.options.noLexicon`
drops the Latin dictionary. For the panels it serves `cue.get` reasons and paradigm cells
for its small lexicon, detects the name Marcus, counts the words of the file, previews and
"writes" exports (a path containing `exists` needs `overwrite`), accepts a `.gguf` path in
`model.locate`, rewrites Latin with simpler words in `orbergise.start`, and records
`shell.*` calls (`VP_MockEngine.shellCalls()`).
B8: the real engine's shapes: `engine.hello` with `engine`, `engineKind`, `pairs` (by default
en-la, es-la, la-en, la-es, en-grc, grc-en, la-la; `options.pairs` replaces the list),
`pairsUnavailable`, `modes`, `model.rerankEnabled:false`, `online {allowed, mock,
mockCalls}`, `samples`; numeric `jobId`; `translate.start` refuses an unavailable pair and
answers `warnings` + `translate.warning` events for a requested model or online check that
cannot run; `correctionAdded` objects; `names.list` without detection
(`options.detectNames` brings "Marcus" back); la-en / grc-en cues with `source-tokens`,
`analysis` reasons and the word-by-word alternative; Greek lemmas (κόρη, ῥόδον, ὁράω,
φίλος) with lemma.get cells incl. the dual and alternative spellings; the Greek sample ends
with a question (`;`).

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
Then (B7) every right-panel tab with the inspector on a word, the Export, Settings and About
dialogs, Orbergise mode on a Latin file, and the six-step tour to its end; listeners and
timers back to the baseline after each. About 25 s.
Then (B8) a reading pair (la-en: source chips, hover card, "Why this reading?", Ctrl+I) and a
Greek target (en-grc: lang="grc", Greek accent, `;`, Gentium Plus in the preview strip and
the cue list by `document.fonts.check` and Chromium's platform-font report, the warning chip
of a requested but missing model opening the Engines tab).
Screenshots in `dev/out/` (gitignored): `start-*`, `workspace-*`, `panel-*`, `dialog-*`,
`orberg-*`, `tour-*` in light/dark, en/es, `reading-la-en-*`, `greek-*`.

    cmake -S . -B build-ui -DVP_BUILD_GUI=OFF -DVP_WITH_LLM=OFF && cmake --build build-ui -j4 --target vpengine
    node gui/ui/dev/smoke_real.js --engine build-ui/engine/cli/vpengine   # Playwright + the real engine on data/work
The real-engine smoke (through `dev/engine_bridge_shim.js`): pair picker = `hello.pairs`, en-la
sample to the end with the Word / Words tabs and the Export preview, la-en with the model and
the online check asked for (warning toast and chips, source chips, card, reading, Ctrl+I,
export preview = readable sentences, chip -> Engines tab), then grc-en, en-grc and la-la when
`hello.pairs` lists them (else "SKIP <pair>" with the engine's reason); no router.leak, no
console errors, listeners back to the Start baseline after each project. Screenshots
`dev/out/real-*.png` (`real-la-en-*`, `real-greek-*`, `real-orberg-*` when available).

## Budgets (PREDESIGN 6.2), reported by tools/jstest
CSS <= 60 KB; JS <= 300 KB gzip in total; fonts <= 2.6 MB; DOM <= 800 nodes; idle heap
<= 60 MB. Screens: `mount(root, params)` / `destroy()` must return VP_Dom, VP_Timers,
VP_I18n, VP_Store, VP_Bridge, VP_Keys and VP_History counts to their pre-mount values.

## Error codes
The engine's codes (DESIGN 9) map to `error.<code>.title` / `.hint`; the bridge adds two
UI-only codes: `timeout` (no answer in time) and `no_engine` (no transport).
