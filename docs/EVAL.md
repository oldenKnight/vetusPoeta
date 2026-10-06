# EVAL — how vetus poeta is measured (task C10; DESIGN §15, PREPLAN §6, DECISIONS D6/D7/D14)

Tools live in `tools/eval/` (Python 3 stdlib; `gt_compare.js` needs Node + Playwright, dev only).
Outputs go to `data/work/` (gitignored). The repo gets numbers and our own sentences only.

## 1. Run the measurement
```
cmake -S . -B build-eval -DVP_BUILD_GUI=OFF -DVP_WITH_LLM=OFF && cmake --build build-eval -j4 --target vpengine
python3 tools/eval/run_eval.py --engine build-eval/engine/cli/vpengine \
    --file tests/regression/own_dialogue.en.srt --gold tests/regression/expected/own_dialogue.la.gold.txt \
    --pair en-la --combos R,R+M,R+O,R+M+O --fidelity 1,2,3 --out data/work/eval/regression/
```
Defaults: engine `build-eval/engine/cli/vpengine`, file = the regression SRT, combos `R,R+M,R+O,R+M+O`, fidelity
`1,2,3`, lexicons from `data/work/` when `latin.vpl` is there (else `tests/fixtures/lex`), 2 runs per cell,
out `data/work/eval/<file stem>/`. Other options: `--setting key=value` (settings patch, e.g. `speakerGender="f"`),
`--primary R/T2` (cell used for the claim, the mismatch table and comparisons), `--tuned` (declare the acceptance
file), `--allow-online`, `--runs N`, `--timeout S`, `--keep-temp`.

Per cell (combination x fidelity) the harness starts a fresh `vpengine serve` with a scratch data folder, sets
`defaultFidelity` and `engines.*`, runs `project.new {sourcePath}`, `cue.page`, `translate.start` (collecting the
`translate.cue` batches and `translate.done.stats`), `cue.get` for every cue (checks A1-A9, tokens, reasons),
`eval.run` (cross-check of the engine's own counts), `export.write` to a temporary file, then reads VmHWM and shuts
the engine down. Each run uses its own engine process, so the determinism hash compares processes, not a cache.

Outputs in `--out`:
- `report.json`: every number as `{num, den, rate, ci95}`; cells (available or not, with the reason), confidence
  distribution, flagged/unflagged split, per-check presence and failure counts, gold match, determinism hashes,
  run time, peak RSS, export identity, provenance (stub engine, tuned file, held-out guard). No cue text.
- `report.md`: the PREPLAN 6.7 publication table, the claim sentence, confidence/flag table, check table, cue
  identity, the comparison section, and the first 40 gold mismatches (source | gold | ours | failed checks).
- `cues_<combo>_T<f>.jsonl`: per-cue records (index, idRaw, source, target, confidence, score, cps, flags,
  checks with ok/detail, unknown tokens, automatic error, flagged, gold match). Input of the review sheet.
- `logs/`: the engine's stderr per cell and run.

Cells that cannot be measured are reported as "not available" with the reason, never silently replaced:
`M` needs `engine.hello.model.available`; `O` needs `--allow-online` (the only way this tool touches the network,
via the engine's own online client) and a passing `online.test`; `M` or `O` alone need the engine to list the mode
in `engine.hello.modes` (no engine does yet: `translate.start` ignores `engines.rules:false`); any
`translate.start` warning (e.g. `model_missing`) marks the cell not available.

## 2. Definitions
- **Counted cue** (PREPLAN 6.1): its text, without `[sound]`/`(sound)` descriptions and music notes, has a letter.
  Sound-only cues are counted separately (`soundOnly`), burned held-out cues too (`burnedExcluded`).
- **Automatic error** (DESIGN 10.4): A1, A3, A4 or A5 fails, a token is unknown, the cue has no output, or the
  target equals the source after normalisation with at least two words ("untranslated English left in",
  PREPLAN 6.1). A check the engine did not report is *absent*, not passed: the report lists absent checks.
- **Flagged**: any check fails, unknown token, no output, copied source, or confidence `check`/`fix`.
- **Gold match**: the target equals any ` | ` alternative of the gold line after normalisation: tags removed,
  NFKD (folds precomposed letters), lower case, every combining mark dropped (macrons, breves), punctuation,
  symbols (emoji) and whitespace collapsed, `v`->`u`, `j`->`i`. Gold format: one line per cue, `#` comments,
  empty line or `-` = no gold.
- **Interval**: Clopper-Pearson exact 95 % (beta quantile by bisection on the regularised incomplete beta).
- **Determinism**: SHA-256 over the targets in cue order, each followed by NUL; `identical` over `--runs` runs.
- **Peak RSS**: VmHWM of the engine process (first run of the cell); `childrenMaxRssKb` is getrusage's maximum.
- **Cue identity**: the export and the source split into blocks; numbering and timing lines byte-identical,
  leading/trailing tags (`<i>`, `{\an8}`) identical, block count and blank-line count equal (ASS: the first nine
  Dialogue fields and the leading override block).
- **"< 1 %"** is never printed by run_eval (automatic checks only). PREPLAN 6.1 allows it only when the upper bound
  on the expert-reviewed sample is below 1 % or every cue was reviewed and fewer than 1 % are wrong; review_sheet
  import prints the sentence when, and only when, that holds.
- Every report states plainly when its numbers come from the **stub engine** (`engine.hello.engine` starts with
  `stub`: it copies the source) and when the file is **tuned** (anything under `tests/regression/`, or `--tuned`).

## 3. Expert review sheet (PREPLAN 6.4)
```
python3 tools/eval/review_sheet.py export --cues data/work/eval/X/cues_R_T2.jsonl --out data/work/review/X.csv
python3 tools/eval/review_sheet.py import data/work/review/X.csv          # key: X.csv.key.json
```
Sample: every flagged cue (census) + a seeded random sample of 300 unflagged cues; rows shuffled; 10 % marked
`second_reader`. Seeds: `--seed` (default 20261006) for the sample, +1 row order, +2 A/B sides, +3 second reader;
all recorded in the key. Columns: row, cue, source, latin, confidence, checks, `error_type` (grammar, meaning,
vocabulary, orthography, markup, none; Spanish names accepted; empty = not reviewed), note, second_reader,
`error_type_2`. Blind A/B: `--b <other cues or system jsonl>`; the sheet shows `latin_A`/`latin_B` with sides
drawn at random per row and no system names; the key file holds the mapping and must not reach the reviewer.
Import computes per system: wrong among flagged and among the sampled unflagged cues (Clopper-Pearson), the
file-level estimate (census + sample rate x unflagged population) and its 95 % upper bound, the rule-of-three
sentence when the sample has no error, the "< 1 %" sentence only when allowed, McNemar exact for A/B (on the
sheet rows, not weighted), Cohen's kappa (categories and wrong/right) for the second reader.

## 4. Held-out hygiene (PREPLAN 6.5, DECISIONS D7)
`--heldout` with a file under `tests/heldout/`: the tool recomputes the SHA-256 of every file listed in
`tests/heldout/FROZEN.sha256`; a changed, missing or unlisted data file makes it **refuse** (exit 3, report.json
says `refused`, no numbers). `tests/heldout/BURNED.txt` (optional): one `<file name> <cue number>` per line (the
file's own cue label); those cues are excluded and counted. A held-out report.md prints no cue text and the
cues jsonl carries no text (reading a held-out cue burns it); `--heldout-texts` keeps texts for the owner's review
sheet only. The owner's second subtitle file is run with `--heldout` from `data/work/` (no frozen hash there).
Always print the acceptance/regression result next to the held-out one, with the tuning caveat.

## 5. Google Translate comparison (PREPLAN 6.6, D6; Latin only)
```
python3 tools/eval/run_eval.py --file F --dump-source data/work/gt/NAME.src.txt
NODE_PATH=/opt/node-tools/node_modules node tools/eval/gt_compare.js data/work/gt/NAME.src.txt --name NAME [--headful]
python3 tools/eval/run_eval.py --file F --gold G --out data/work/eval/NAME/ \
    --compare-system data/work/gt/NAME.gt.la.txt --system-name "Google Translate"
```
`gt_compare.js`: one Chromium context, realistic UA, batches of up to 25 lines (blank-line separated, at most
4,500 characters and 7,000 URL-encoded characters). Each batch is submitted by loading
`...?sl=en&tl=la&op=translate&text=<encoded batch>`; when no result appears within `--wait-ms` (30 s) the text is
typed into the source box (ARIA "Source text"), checked to hold the whole batch, and the result awaited again.
Result = the `lang="la"` element of the target panel, else the polite live region; the placeholders "Translation" /
"Translating..." are no result. Output split on blank lines; one line per request when counts differ. 1.5-3 s
jittered pause between requests, no parallelism. No result -> stops with `data/work/gt/NAME.failure.png`. CAPTCHA
and consent walls stop the run; with `--headful` the owner completes them in the window (5 min wait). Sound-only
lines (`[music]`, notes) are copied, not sent. Output `NAME.gt.la.txt` (one line per input line) + `NAME.gt.meta.json`
(requests, fallbacks, typed batches). `--base-url` points the tool at `tools/eval/tests/fixtures/gt_mock.html`
(tests, offline, no pause).
Scoring: each line of the other system goes through `cue.set` on a scratch project of the same file (the server
runs `Engine::check` on it), then `cue.get` gives checks and tokens; automatic error = A1, A3 or A4 fails, unknown
token, no output or copied source (A5 and A6 reported, not counted; plain text has no markup and no tier
promise). Our side uses the same definition on the `--primary` cell. Paired McNemar exact tests on discordant
pairs (automatic error; gold mismatch). `compare.html` shows per-cue A/B rows; `system_<name>.jsonl` holds the
other system's records (input of a blind A/B review sheet).
Dry runs 2026-10-06. (1) First version, live: Chromium in this container cannot open HTTPS pages through the
session proxy (`ERR_CERT_AUTHORITY_INVALID`, the browser certificate store lacks the proxy CA); a plain fetch showed
`<textarea aria-label="Source text">` and a `lang="en"` source panel. (2) A live run with certificate checking turned
off (by the coordinator, not committed) loaded the page but the typed batch arrived as its last line only and nothing
was translated; hence the URL submission above. (3) Rewritten flow, offline against the mock page: 5 lines in one
URL batch, split correctly; per-line fallback on a merged result; typing fallback; 30 regression lines -> batches
of 25 + 5, all split correctly (tests `GtCompareMock`). **Not re-run live**: this repo's tool does not switch off
certificate checking, so the live check of the rewritten flow (result locator, splitting of real output) is for the
owner on a normal machine:
`node tools/eval/gt_compare.js data/work/gt/dry5.src.txt --name dry5 --limit 5`.

## 6. Never committed
Google Translate output (`data/work/gt/`), compare.html, system_*.jsonl, cues_*.jsonl and review sheets of the
owner's or held-out files, the owner's subtitle files. run_eval.py, review_sheet.py and gt_compare.js refuse to
write inside the repository except under `data/work/`. Numbers (report.json) may be quoted in docs.

## 7. Tests
`python3 -m unittest discover -s tools/eval/tests` (normalisation, intervals vs known values, McNemar, kappa,
held-out guard on a tampered copy, structure check, review sheet round trips, gt_compare.js helpers, and an end-to-
end run of run_eval.py against the built engine incl. a held-out run and a system comparison; set `VP_ENGINE` to
pick the binary, skipped when none is built).
