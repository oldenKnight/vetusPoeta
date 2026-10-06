<p align="center">
  <img src="assets/logo_wordmark.svg" alt="vetus poeta" width="440">
</p>

<p align="center"><b>An offline Latin translator for a teacher and the students in the class.</b><br>
Windows 10/11 · works with the network unplugged · no account · English and Spanish (Mexico) interface ·
proprietary, source visible for review</p>

vetus poeta turns English or Spanish subtitle files and short texts into Latin a beginner can read (Familia Romana
style: core vocabulary, indicative main clauses, verb-final order, macrons shown), and Latin texts into English or
Spanish with an interlinear view under every word. Subtitle timing, numbering and styling codes are copied byte for
byte; only the text changes. It never guesses silently: every cue carries a mark (OK, Check, Fix) and every word can
show why it was chosen, from which candidates, in which form and on what evidence. The translation is done by a
deterministic rule engine over dictionaries built from Wiktionary and other open sources; a small local language
model and an online Wiktionary check are optional helpers that never write the Latin. Ancient Greek and an
"Orbergise" mode (rewrite Latin into simpler Latin) are designed and partly built, but not available in 0.1.0.

![The workspace: cue list, English source, Latin translation, player preview and the Word tab](docs/screenshots/word-inspector.en.png)

## Features

- **Subtitles into Latin** from English or Spanish: `.srt`, `.vtt`, `.ass`/`.ssa`, plain text; numbering, timing,
  `<i>`, `{\an8}` and ASS override blocks untouched; lines re-broken at 42 characters, at most 2 lines; reading-speed
  warnings (17 / 20 characters per second, adjustable).
- **Latin into English or Spanish** with an interlinear line per word (dictionary form, form in words, meaning) and
  the role of each word in the sentence.
- **Review flow**: OK / Check / Fix marks with shape, colour and word; filters; accept, edit, three alternatives per
  cue; undo/redo; "Accept all green".
- **"Why this word?"** for every word: meaning used, ranked candidates with word level, the form in words with
  grammar help (both languages), the full table of forms, evidence from Wiktionary, Whitaker's Words, the model and
  the online check, and the nine automatic checks of the cue.
- **Fidelity slider**: extremely faithful (any word) / balanced (common words) / flexible (basic words, may
  rephrase); the meaning is never traded.
- **Names glossary** (keep, decline, translate), **correction memory** (applied before the rules on later cues),
  **words in the file** with level badges, copy as list or CSV for flashcards.
- **Emoji after picturable nouns** in the app (off in the exported file by default), **macrons** shown in the app
  (off in the exported file by default).
- **Export** with checks (cue count, timing unchanged, fast cues, cues still to fix), UTF-8 / UTF-16 / Windows-1252,
  optional BOM, preview.
- **Projects** (`.vpoeta`) with atomic save, autosave, crash recovery; the window supervises the translator process
  and restarts it with the project restored.
- **Optional local model** (CPU only, loaded per job) and **optional online check** (off by default; sends single
  dictionary words to wiktionary.org and nothing else).
- First-run tour, keyboard shortcuts for everything, light and dark themes, Gentium Plus for Latin and Greek.

User guide: **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)** · en español:
**[docs/USER_GUIDE.es-MX.md](docs/USER_GUIDE.es-MX.md)**.

## Status (version 0.1.0, 2026-10-06)

The task ledger is [docs/STATUS.md](docs/STATUS.md); decisions are in [docs/DECISIONS.md](docs/DECISIONS.md).

| Part | State |
|---|---|
| Build, cross-compile, sanitizers (A1) | done; Linux and MinGW builds; the MSVC build is not verified yet |
| Engine core: text, project file, settings (A2); subtitle I/O (A3) | done |
| Dictionary library: Latin, Greek, English, Spanish (A4, B4, B4b); reader (B1) | done |
| English and Spanish sentence analysers (B2) | done |
| Rule engine English/Spanish → Latin (C1, C2, C2b, C13) | done |
| Rule engine Latin → English/Spanish (C11) | done |
| Greek side: morphology, realiser, checker (C9) | done; the English/Spanish ↔ Greek engine path (C12) is open |
| Orbergise (C14) | open: the screen exists, the engine part does not |
| Engine server `vpengine` (B5, C8) | done |
| Interface (B3, B6, B7) | done; the follow-up against the real engine (B8) is in progress |
| Windows shell (C3), brand (C4) | done; cross-compiled only, not yet run on Windows (owner checklist in [docs/BUILD.md](docs/BUILD.md)) |
| Local model (C5), online check (C7), measurement harness (C10) | done |
| Critic pass, Google Translate comparison, release packaging (M7) | open |

Measured numbers. The regression files are our own sentences and **were used to tune the rules** (DECISIONS D14),
so they are optimistic; the numbers that matter (held-out and acceptance) are not measured yet.

| Measure | Result |
|---|---|
| English → Latin, 114-cue regression file, balanced fidelity | 114/114 match the reference (110/114 before four engine outputs were accepted as alternative references); marks OK 69, Check 45, Fix 0 |
| Spanish → Latin, 100-cue regression file | 97/100 at the end of the Spanish loop; the 3 others were then accepted as alternatives (re-run 2026-10-06: 100/100); marks OK 78, Check 22, Fix 0 |
| Latin → English and Latin → Spanish, 125 own sentences | 125/125 in both (tuned; a batch of 30 written later was 20/30 on its first run) |
| Greek realiser, first 40 regression lines | 40/40 against the tuned Greek reference, from hand-built clause structures (no end-to-end Greek translation yet) |
| Held-out set (`tests/heldout/`) | **not measured yet** |
| Acceptance test (the owner's film subtitle file) | **pending**: the file has not arrived |
| Google Translate side-by-side (Latin) | **pending**: the tool is ready and tested offline; the live run has not been done |
| Local model, Latin minimal-pair gate (1,000 pairs) | 749/1,000 = 74.9 % (95 % Wilson 72.1-77.5 %), below the 75 % bar fixed in advance: **not passed**, so the model is used for English/Spanish understanding only |
| Sentence analysers (UD test sets) | English UPOS 94.62 %, LAS 79.65 %; Spanish UPOS 96.71 %, LAS 79.39 % |
| Determinism and subtitle identity | two engine processes byte-identical; exported numbering, timing and tags byte-identical to the source |
| Engine memory (Linux) | peak about 140 MB on a 114-cue file; resident memory flat over 1,000 cues |

## Platform and minimum hardware

Windows 10 or 11, 64-bit, with the Microsoft Edge WebView2 Runtime (part of current Windows). Target for all three
engines (decision D4): Intel Core i3 with AVX2 (4th generation or newer), 4 GB RAM, integrated graphics (unused),
about 800 MB of disk with the optional model (the portable folder is about 240 MB without it; the model file is
about 400 MB). Planned peak memory: under 250 MB without the model, under 1.2 GB with it. Development and all
measurements so far are on Linux; nothing has been timed on an i3 yet.

## Build and test

Everything is in **[docs/BUILD.md](docs/BUILD.md)** (Linux, sanitizers, MinGW cross build, MSVC, the WebView2 SDK,
the portable folder, the model file). In short, on Linux:

```sh
cmake -S . -B build -DVP_BUILD_GUI=OFF          # add -DVP_WITH_LLM=OFF to skip llama.cpp
cmake --build build -j4
ctest --test-dir build --output-on-failure
tools/jstest.sh                                 # interface tests (Node >= 18)
tools/xcompile_check.sh                         # Windows cross build with MinGW
python3 tools/make_dist.py --mingw build-mingw-release    # portable folder dist/vetus-poeta/
```

How quality is measured: [docs/EVAL.md](docs/EVAL.md). The teacher's guide to the word levels:
[docs/TEACHER_REVIEW.md](docs/TEACHER_REVIEW.md).

## How the dictionaries are built

`tools/build_library/` (Python standard library only) turns the raw dumps into four memory-mapped dictionary files
(`latin.vpl`, `greek.vpl`, `english.vpl`, `spanish.vpl`): Wiktionary through the Kaikki/Wiktextract extracts,
Lewis & Short and LSJ from the Perseus Digital Library, the Dickinson College Commentaries core vocabularies and
Whitaker's Words, plus the hand-written tables in `data/curated/` (word levels, Spanish glosses, phrasebooks, name
tables, word order rules). The raw data and the built files are never committed (`data/raw/`, `data/work/`). The
steps, stages and checks are in [tools/build_library/README.md](tools/build_library/README.md); every file carries
its sources' licence notice, which the app shows under About. The sentence analysers are trained by `tools/train/`
on Universal Dependencies treebanks (UD English-EWT, UD Spanish-GSD and AnCora). Licences of everything not written
for this project: [third_party/LICENSES.md](third_party/LICENSES.md).

## Repository layout

```
engine/   C++17 engine: core (text, files, project, settings), lex (dictionary reader), subs (subtitle I/O),
          nlp (tagger, parser), rules (morphology, transfer, Latin and Greek realisers, checker, la2x),
          llm (local model), online (Wiktionary check), cli (vpengine: JSON-lines server), tests
gui/      shell (Win32 + WebView2 host), ui (ES5 interface, i18n/en-US.json and es-MX.json, fonts)
tools/    build_library, train, eval, jstest, make_dist.py, make_icons.py, pack_ui.py, xcompile_check.sh
data/     curated/ (hand-written tables, committed); raw/ and work/ (downloads and builds, not committed)
tests/    regression/ (tuning set and references), heldout/ (frozen), samples/, fixtures/
assets/   logo, icon, splash (original artwork)        models/   README only (the model file is not committed)
docs/     DESIGN, DECISIONS, STATUS, BUILD, EVAL, user guides, notes per module
third_party/  vendored nlohmann/json, miniz, doctest, llama.cpp (CPU subset) and LICENSES.md
```

## Licence

vetus poeta is **proprietary software**: © 2026 oldenKnight, all rights reserved ([LICENSE](LICENSE)). The source
is visible for review; building and running it for personal, non-commercial evaluation is permitted; any other use
needs the owner's written permission. The artwork in `assets/` is original and proprietary like the rest.

Data and code by others keep their own licences, listed in [third_party/LICENSES.md](third_party/LICENSES.md) and
shipped as `THIRD_PARTY_NOTICES.txt`: Wiktionary data via Kaikki (CC BY-SA 4.0), Perseus Lewis & Short and LSJ
(CC BY-SA 4.0), DCC Core Vocabularies (CC BY-SA 3.0), Whitaker's Words (author's permission), Universal Dependencies
treebanks (CC BY-SA / CC BY), Gentium Plus (SIL Open Font License 1.1), llama.cpp, nlohmann/json, miniz and doctest
(MIT), the optional Qwen2.5-0.5B-Instruct model file (Apache-2.0), and Microsoft Edge WebView2 (Microsoft's terms).
Changes are listed in [CHANGELOG.md](CHANGELOG.md).
