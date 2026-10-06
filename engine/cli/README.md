# engine/cli (`vpengine`)

The engine executable the shell starts as a sidecar. Protocol: DESIGN.md §9 (JSON lines), views §9.2, settings §9.1.
Links `vp_core`, `vp_subs`, `vp_lex`, `vp_rules` (engine i, `makeEngine(EngineConfig)`; `-DVP_HAVE_RULES=OFF`
builds without it), `vp_llm` (engine ii; a header stub with `-DVP_WITH_LLM=OFF`) and `vp_online` (engine iii, the
only network code). The echo engine of `src/rules_stub.cpp` stays for protocol tests (`--stub`, `VP_FORCE_STUB=1`).

```
vpengine serve [--data <dir>] [--lexicons <dir>] [--stub]   JSON lines on stdin/stdout, log on stderr (VP_LOG)
vpengine inspect <latin.vpl|greek.vpl> <word>      analyses + paradigm cells (latin_key / greek_key by script)
vpengine check <file.srt|.vtt|.ass|.txt> [--cps N] format, encoding, cue count, warnings, cues read too fast
vpengine llm-gate <latin.vpl> <model.gguf> ...     Latin minimal-pair gate of the local model (C5)
vpengine version
```

## Data layout (dist) and discovery (`src/locate.cpp`)
```
<dist>/vpengine.exe, VetusPoeta.exe
<dist>/data/latin.vpl greek.vpl english.vpl spanish.vpl     lexicons (the shell passes --lexicons <dist>/data)
<dist>/data/nlp/english.tag.vpt english.dep.vpt spanish.tag.vpt spanish.dep.vpt   source analysis (B2)
<dist>/data/curated/*.tsv *.txt                              rule tables (order_la.txt, phrasebook, tiers ...)
<dist>/samples/sample.en.srt sample.es.srt sample.la.srt sample.grc.srt   12 cues each, our own sentences
<dist>/models/<model>.gguf                                   optional local model (C5)
```
`tools/make_dist.py` writes this layout. Lexicons: `--lexicons`, else `VP_LEXICON_DIR`, else `<exe>/data` (when it
holds latin.vpl), `<exe>/lexicons`, `<data>/lexicons`. NLP models: `VP_NLP_DIR` (used as given), else
`<lexicons>/nlp`, `<exe>/data/nlp`, then `data/work/nlp` in the folders above the executable (source tree).
Curated tables: `VP_CURATED_DIR`, else `<lexicons>/curated`, `<lexicons>/../curated`, `<exe>/data/curated`, then
`data/curated` above the executable. Samples: at start every `sample.<lang>.srt` missing from `<data>/samples/` is
copied there from `VP_SAMPLES_DIR`, `<exe>/samples` or `tests/samples` above the executable, so the UI's
`<dataDir>/samples/sample.<lang>.srt` exists; `engine.hello.samples` lists the paths.

## engine.hello (additions to DESIGN 9)
`engine` (version string), `engineKind` "rules" | "stub", `lexicons[].tiers {t1,t2,t3}` (lemma counts of latin.vpl
and greek.vpl), `pairs` (the pairs translate.start accepts now), `pairsUnavailable [{pair, code, message, hint}]`
(all nine pairs of DESIGN 9 are listed in one of the two: en-la es-la la-en la-es en-grc es-grc grc-en grc-es la-la;
`lexicon_missing` without latin.vpl / greek.vpl (Greek pairs: "The Greek dictionary (greek.vpl) is not installed.");
en/es -> la/grc: `not_found` without the curated tables or without `<base>.tag.vpt/.dep.vpt` - the hint names the
files; every other case: what the rules engine answered to a one-cue probe), `modes` ("R" when a pair is available, "M"
when the model file is found and the CPU is supported, "O" always: engine iii is built in and runs only when the
settings turn it on), `model {..., rerankEnabled, reason}`, `online {allowed, mock, mockCalls}`,
`nlp {dir, en, es}`, `curatedDir`, `samples [{lang, path}]`, `dataDir`, `lexiconDir`, `threads`.
`translate.start` / `orbergise.start` on an unavailable pair fail at once with that code and hint.

## Engines ii and iii in a job (`src/server_engine.cpp`)
The rules engine is built once with `EngineConfig{dataDir=<lexicons>, curatedDir, nlpDir, advisors}`. Its
advisors forward to the running job only: `chooseSense` loads the model lazily on the engine's first question
(`vp::llm::makeAdvisors`), the model is unloaded when the job ends; `onlineCheck` creates the Wiktionary client
on first use when `engines.online && online.wiktionary` (re-read at every call; cache `<data>/online-cache`) and
answers `Evidence{+1|-1|0, "online", "<lemma>: <summary> <url>"}`. After a model load failure or an online error
the job goes on without that engine. A requested engine that cannot run is never dropped silently: the
`translate.start` result keeps `warnings: [code...]` and the job sends one event per engine
`translate.warning {jobId, engine:"model"|"online", code, message, hint}` (at the start: model_missing,
model_unsupported_cpu, model_load_failed, online_disabled; during the job: the load or network failure).
`VP_ONLINE_MOCK=1` (tests only) swaps the transport for a scripted one (`src/online_mock.cpp`: the lexicon's own
gloss as the definition, `=disagree` an unrelated one, no socket, 1 ms throttle).

## Per-cue engine data and the views the UI reads (DESIGN 9.2)
`vp::CueRecord` has no fields for tokens, flags or job facts, so they are stored as hidden CueReasons
(`_tokens` compact token list, `_flags`, `_job` {model, online, orberg, heads the model chose, online verdicts})
inside the record: saved in the project, restored by undo/redo, never shown. `cue.get` serves the stored tokens
(Engine::check re-analyses only when none are stored), so every `reasons[].tokenIndex` points into `tokens`.
Reasons are turned into the UI shapes at `cue.get`: sense `{source, sense, senseEs, context[]}`; one `candidate`
reason per candidate `{lemmaId, head, form, tier, band:"common"|"rarer"|"rare", chosen, gloss, score}` (chosen first,
`form` of the others generated with the token's features); form `{features, form?}`; per dictionary token four
evidence rows `{source:"wiktionary"|"whitaker"|"model"|"online", state:"yes"|"no"|"off"|"none"}` (wiktionary = the
lexicon has a gloss, whitaker = a Whitaker frequency letter, model/online from the job facts); the engine's
sentence-level evidence (`model: ...`, `online: ...`) as `{source, state, preferred?}`; name `{form}`; correction
`{target}`; Orbergise: the engine's reasons of kind `orbergise` pass through (`data {was, now, why}` for a change,
no data for a note); only the stub's output gets changes derived by aligning the input Latin with the output. In
Orbergise mode (pair la-la or an orbergise job) `cue.get` adds `meaning {percent, missing[]}` and `original` (see
"Orbergise" below). CueView `flags`: the engine's (`emoji`, `unknownName`, `song`, `nonverbal`, `name-guessed`,
`frame-fallback`, `tags`, ...) plus `cps`, `overflow`, `alternative` recomputed from the current target.
For la-en / la-es (C11) the tokens describe the Latin source words (flag `source-tokens`) and the reasons of kind
`analysis` pass through unchanged; the evidence rows and `words.list` use the Latin/Greek side's lexicon.
`LemmaView.flags` adds `gloss-es-pivot` (lexicon flag bit 8). `words.list` counts the engine's tokens (lemma ids,
effective tiers; capitalised unknown words as names). `settings.speakerGender` "m" | "f" | "u" ->
`Options.speakerGender`. `CueInput.spans` carries the source cue's text/tag spans (newline = text "\n").
`cue.set {text, remember}` with the text the cue already has and state `edited` only adds the correction (no
history step, no re-check); any other edit re-checks the cue, stores the new tokens and re-attaches the reasons
to the words that are still there. `cue.choose` does the same with the alternative's text.

## Orbergise (`orbergise.start`, pair la-la; C8b)
`orbergise.start {indices?, tier:1|2, keepNames?:true, simplify?:true, originalPath?, originalLang?:"en"|"es"}` ->
`{jobId, total, warnings, originalPath|null, originalLang|null}`; the same `translate.*` events as a translation.
Options: `orbergTier = tier`, `orbergKeepNames = keepNames`, `orbergSimplify = simplify` (both default true).
`originalPath` loads the original-language file (srt/vtt/ass/ssa/txt through vp::subs); it stays loaded for later
runs; `originalPath: ""` forgets it; absent/null keeps what is loaded. Alignment (`alignOriginal`, views.h):
1. the same number of cues, or either file untimed (.txt): by cue index (position);
2. else by time: every original cue overlapping the cue for at least half of the shorter of the two, in order,
   joined with a space; a cue no original covers that well gets the original cue with the largest overlap; no
   overlap at all -> "" (that cue is rewritten from the Latin alone).
`originalLang` absent: detected (`detectOriginalLang`: ¿ ¡ ñ and accented vowels plus common Spanish function words
against common English function words; ties -> "en"). Each cue's `CueInput.originalText/originalLang` come from it.
The path and language are kept in the manifest settings snapshot (`orbergOriginalPath`, `orbergOriginalLang`) and
restored by `project.open` (a missing file -> a `warnings[]` entry); `project.orberg {originalPath, originalLang}`
reports them. Per cue the engine's `CueOutput.meaningPercent/meaningMissing/original` are stored as the hidden
reason `_orberg`; `cue.get.meaning {percent, missing}` is the engine's when percent >= 0, else (after a user edit,
or the stub) the CLI's content-lemma overlap with the input Latin; `cue.get.original` is the loaded file's aligned
cue, else the one the job used.

## Export of Greek
`export.write` / `export.preview {greek:"monotonic"}` run each Greek target through `vp::grc::toMonotonic` (C12:
one tonos per word, breathings, iota subscript and length marks dropped, diaeresis kept, monosyllables unaccented
except ή and the interrogatives ποῦ ποῖ πῶς πῇ τίς τί); only for a Greek target pair, polytonic by default. Numbering,
timing and tags are untouched like every same-format export. (Without the rules engine, a local approximation.)

## Threads and framing (`src/server.h`)
Reader thread: parses each line; answers `engine.ping`, `engine.shutdown` and `*.cancel` itself; queues the rest
(cap 1,024, full -> `busy`). A malformed line gets `{"id":null,"ok":false,"error":{"code":"bad_params",...}}`.
Worker (the main thread): one command at a time, translate/orbergise jobs inline (engine called per 20 cues,
`translate.cue` per batch, `translate.progress` at most every 100 ms, cancel per cue), autosave check every 1 s
(`project.autosaved {path, at}`). Shutdown/EOF: queued requests still run, jobs stop, dirty project autosaved, lock
removed. One `Output` writes every line under a mutex. Every command catches at the boundary (`internal` + hint).

## Choices the design left open (UI authors read this)
- `index` is the 0-based cue position everywhere (`idRaw` is the file's label); `cue.page` `count` <= 200.
- `translate.cue` carries `cues:[CueView]` (a batch), not `cue`; `translate.done {stats:{done, translated, total,
  cancelled, ok, check, fix, ms, cuesPerSec}}`. Orbergise emits the same `translate.*` events.
- CueView `confidence` is `check` until translated (`state:"new"`); `flags`: `cps`, `overflow`, `alternative`.
- `project.new` without `path` autosaves under `<data>/unsaved/`; `project` = `{path, autosavePath, manifest,
  stats, subs:{format, encoding, bom, newline, warnings}, dirty, canUndo, canRedo}`.
- `project.close {discard?}` deletes the autosave only when `discard` is true; `project.save` removes it.
- `export.write` to an existing file: `io` error with message `file exists: ...` unless `overwrite:true`.
  Same format = source document with text spans replaced (numbering, timing, header, layout byte for byte);
  tags before/after the text kept, inner tags dropped (warning `tags_dropped`), `<i>` etc. closed again.
  Other format: new document, timing converted. Untranslated cues keep their source text (`untranslated`).
  `emoji:false` drops pictographs absent from the source (music notes stay); `greek:"monotonic"` see below.
- `translate.start` without `indices` skips edited and reviewed cues; `engines.model/online` yield `warnings` and
  `translate.warning` events (above).
- `model.*` answer `available:false` / `model_missing`; `online.test` gives `online_disabled` or `online_failed`.
- History: cue-level before/after records of `cue.set/choose/review` and jobs, 500 steps.

## Run it by hand
EOF on stdin cancels running jobs, so keep stdin open a moment after the heredoc:
```
(cat <<'EOF2'; sleep 3) | ./build/engine/cli/vpengine serve --data /tmp/vp --lexicons data/work
{"id":1,"cmd":"engine.hello"}
{"id":2,"cmd":"project.new","params":{"kind":"subs","pair":"en-la","sourcePath":"tests/samples/sample.en.srt"}}
{"id":3,"cmd":"translate.start","params":{}}
{"id":4,"cmd":"word.inspect","params":{"text":"puellam","lang":"la"}}
{"id":5,"cmd":"export.write","params":{"path":"/tmp/vp/out.srt","overwrite":true}}
EOF2
```

## Tests
`engine/tests/test_server.py` (ctest `vpengine_server`): with the stub engine (`VP_FORCE_STUB=1`) session, framing,
10,000-ping burst, ping during a 5,000-cue job, cancel, kill -9 + recover, EOF, no network symbols; with the rules
engine on `data/work` (skipped with a message when the lexicons or NLP models are absent) the sample session
(Latin for 12 cues, reason shapes, word.inspect, words.list, export byte for byte with `{\an8}` and italics,
names.set, corrections, speaker gender, unavailable pairs), missing NLP models, Orbergise on sample.la.srt
without and with sample.en.srt/sample.es.srt (detection, a 5-cue mismatched original aligned by time, edit, reopen
with the original restored, `originalPath: ""`), en-grc / grc-en on the samples, monotonic export (timing bytes
untouched), the pair report without greek.vpl, the online check through
`VP_ONLINE_MOCK`, and a three-cue local-model run (skipped without a model file, under sanitizers, or with
`VP_LLM_SKIP_MODEL=1`). `--stub-only` / `VP_TEST_STUB_ONLY=1` runs the stub parts only.
`engine/tests/test_cli.cpp` (`vp_tests -tc='cli*'`). UI against the real engine: `node gui/ui/dev/smoke_real.js
--engine <vpengine>` (through `gui/ui/dev/engine_bridge_shim.js`, which can also be started by hand).
