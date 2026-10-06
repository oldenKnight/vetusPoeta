# engine/cli (`vpengine`)

The engine executable the shell starts as a sidecar. Protocol: DESIGN.md §9 (JSON lines), views §9.2, settings §9.1.
Links `vp_core`, `vp_subs`, `vp_lex` and, until engine/rules lands, the stub engine in `src/rules_stub.cpp`
(`-DVP_HAVE_RULES=ON` switches to `vp::rules::makeEngine()`). No network code.

```
vpengine serve [--data <dir>] [--lexicons <dir>]   JSON lines on stdin/stdout, log on stderr (VP_LOG=off..debug)
vpengine inspect <latin.vpl|greek.vpl> <word>      analyses + paradigm cells (latin_key / greek_key by script)
vpengine check <file.srt|.vtt|.ass|.txt> [--cps N] format, encoding, cue count, warnings, cues read too fast
vpengine version
```
Defaults: data folder `fs::dataDir()` (`VP_DATA_DIR`); lexicons from `VP_LEXICON_DIR`, else `<exe dir>/lexicons`,
else `<data>/lexicons`. `VP_AUTOSAVE_MS` shortens the 5 s autosave debounce (tests).

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
  `emoji:false` drops pictographs absent from the source (music notes stay); `greek:"monotonic"` is approximate.
- `translate.start` without `indices` skips edited and reviewed cues; `engines.model/online` yield `warnings`.
- `model.*` answer `available:false` / `model_missing`; `online.test` gives `online_disabled` or `online_failed`.
- History: cue-level before/after records of `cue.set/choose/review` and jobs, 500 steps.

## Run it by hand
EOF on stdin cancels running jobs, so keep stdin open a moment after the heredoc:
```
(cat <<'EOF2'; sleep 1) | ./build/engine/cli/vpengine serve --data /tmp/vp --lexicons tests/fixtures/lex
{"id":1,"cmd":"engine.hello"}
{"id":2,"cmd":"project.new","params":{"kind":"subs","pair":"en-la","sourcePath":"tests/fixtures/cli/thirty_cues.srt"}}
{"id":3,"cmd":"translate.start","params":{}}
{"id":4,"cmd":"word.inspect","params":{"text":"puellam","lang":"la"}}
{"id":5,"cmd":"export.write","params":{"path":"/tmp/vp/out.srt","overwrite":true}}
EOF2
```

## Tests
`engine/tests/test_server.py` (ctest `vpengine_server`: session, framing, 10,000-ping burst, ping during a 5,000-cue
job, cancel, kill -9 + recover, EOF, no network symbols) and `engine/tests/test_cli.cpp` (`vp_tests -tc='cli*'`).
