# STATUS — task ledger (resume from here after any crash)

Legend: TODO · WIP(agent) · REVIEW · DONE · BLOCKED. Only the main agent moves a task to DONE.
Subagents: do exactly one task, in your own files, then set REVIEW and commit **only your paths**
(`git add <your files>`; if `.git/index.lock` exists, wait 5 s and retry). Never `git add -A`.
Never push. Never spawn subagents.

## Milestones
- M0 skeleton + decisions + pre-plan (step 1 of the production process)
- M1 offline library built from Wiktionary (Latin, Greek, English pivot, Spanish)
- M2 engine core: lexicon, morphology, subtitle I/O, project format, JSON-lines server
- M3 rule engine EN/ES -> LA, acceptance loop on Alice, held-out measurement
- M4 UI + shell, i18n, logo, font
- M5 local model (engine ii) + online check (engine iii)
- M6 LA -> EN/ES, Greek, Orbergise
- M7 critic pass, Google Translate side-by-side, release packaging

## Tasks
| ID | Module | Task | Owner | State | Notes |
|---|---|---|---|---|---|
| M0.1 | docs | Repo skeleton: CLAUDE.md, DECISIONS.md, STATUS.md, HANDOFF.md, .gitignore | main | DONE | 2026-10-05 |
| M0.2 | data | Background download of Kaikki Latin, Ancient Greek, English and es.wiktionary extracts into data/raw (gitignored) | main | DONE | logs in data/raw/dl-*.log, flag data/raw/dl-done.flag |
| M0.3 | docs | Sonnet pre-plan and pre-design: docs/PREPLAN.md, docs/PREDESIGN.md | sonnet | REVIEW | written 2026-10-05; numbers measured with throwaway scripts on data/raw (M1.1 must reproduce them); main agent redoes freely afterwards |
| M0.4 | docs | DESIGN.md v1 (contracts) written by main agent from the pre-work | main | DONE | plus CMake skeleton, third_party seeds, result.h, features.h/.py |

### Wave A (opened 2026-10-06; DESIGN.md §16)
| ID | Module | Task | Owner | State | Notes |
|---|---|---|---|---|---|
| A1 | BUILD | Top-level CMake (presets, sanitizer, VP_WITH_LLM), vendor llama.cpp pinned minimal tree as target `vp_llama`, `tools/xcompile_check.sh` (MinGW installed on this box), `docs/BUILD.md`, LICENSES.md current. Owns: CMakeLists.txt, CMakePresets.json, cmake/, third_party/ (not the module dirs), tools/xcompile_check.sh, docs/BUILD.md | opus | TODO | DESIGN §2, §3, §11; PREPLAN 2.3 has the tested configure line and the scratch clone path |
| A2 | CORE | engine/core: text.h normalisation (§4) with C++ NFC subset, errorCodeName, fs utils + atomic write, MappedFile RAII (Win32 + POSIX), settings store, project .vpoeta (§8) ported from the prototype's project.cpp (zip, atomic save, autosave debounce, lock + recovery, corruption handling, migration stub); tests test_core.cpp, test_project.cpp; fixtures tests/fixtures/project/ | opus | TODO | reads tests/fixtures/normalisation_golden.tsv (written by A4) when present |
| A3 | SUBS | engine/subs per §7: SRT/VTT/ASS/TXT parse + byte-exact write, encodings, tag spans, breakLines, charsPerSecond, warnings; test_subs.cpp; fixtures tests/fixtures/subs/ (own sentences only) | opus | TODO | |
| A4 | LIB | tools/build_library stages `fetch`, `kaikki`, `resolve` (+ vptext.py per §4, features.py packing, tests/fixtures/normalisation_golden.tsv >= 200 rows, tests/fixtures/features_golden.tsv), report.json, `build.py --check` with expected_counts.json, unittest fixtures with hand-made entries | opus | TODO | raw dumps in data/raw/; outputs data/work/{la,grc,en,es}/ |
