# Changelog

All notable changes to vetus poeta. Dates are in ISO format. The task ledger with every measurement is
[docs/STATUS.md](docs/STATUS.md).

## 0.1.0 - 2026-10-06

The first version: English and Spanish subtitles and texts into Latin and Ancient Greek, Latin and Greek into English
and Spanish, and the Orbergise mode, fully offline. Built and tested on Linux, cross-compiled for Windows; not yet
run on Windows.

### Milestones

- **M0, plan and contracts (2026-10-05).** The owner approved the plan and the binding decisions (name, platform,
  proprietary licence with open data only, minimum hardware, the role of the local model, the Google Translate
  comparison, held-out testing, Spanish glosses, emoji, fidelity, orthography, error metric, subtitle formats,
  autosave, interface languages). A pre-plan and a pre-design were written and the design contract followed.
- **M1, the offline library.** The raw Wiktionary extracts (Latin, Ancient Greek, English, Spanish) are turned into
  four memory-mapped dictionary files by `tools/build_library`, joined with Whitaker's Words, Lewis & Short, LSJ and
  the Dickinson College core vocabularies, with English and Spanish glosses, word levels (tiers) and emoji for
  picturable nouns. A follow-up fixed Greek particles, final sigma, article-bearing and romanised table cells,
  dialect flags and deponents, added Whitaker-only Latin forms, cut the English dictionary from 68.6 to 9.3 MB and the
  Spanish one from 41.2 to 23.8 MB, and produced review sheets so the teacher can set the word levels.
- **M2, the engine core.** Text normalisation, the `.vpoeta` project file (zip, atomic save, autosave, lock and
  recovery), settings, the dictionary reader, subtitle reading and writing (SRT, VTT, ASS/SSA, text; byte-exact
  numbering, timing and tags; encodings), English and Spanish part-of-speech taggers and parsers trained on
  Universal Dependencies, and the `vpengine` JSON-lines server with a worker thread, cancel and recovery after a
  crash.
- **M3, the rule engine into Latin.** Latin morphology and realisation (agreement, word order from
  `data/curated/order_la.txt`, negation, questions, imperatives, names) with a checker that re-analyses its own
  output; the source side (frames from the parse, phrasebook, contractions, cue and sentence mapping) and lexical
  transfer by sense, word level, valency and corrections. Two quality loops: English 75 → 110 of 114 regression cues
  (114 after four engine outputs were accepted as alternative references), Spanish 15 → 97 of 100 (the last 3 then
  accepted the same way). Both files were used for tuning; the held-out set and the owner's acceptance file are not
  measured yet.
- **M4, interface, shell and brand.** The ES5 interface in English and Mexican Spanish (start screen, virtualised
  cue list for 50,000 cues, source and translation panes, preview, alternatives, Word tab with "Why this word?" and
  tables of forms, Engines, Names, Corrections, Words, Export, Settings, About, a six-step tour), the Win32 +
  WebView2 shell that supervises the engine, and the original "arch and macron" logo, icon and wordmark.
- **M5, the optional engines.** The local model engine on vendored llama.cpp (CPU only, loaded per job, SHA-256
  checked). Its Latin gate was not passed (74.9 % on 1,000 minimal pairs against a 75 % bar), so it only helps
  with English and Spanish source understanding. The online Wiktionary check is off by default, sends single
  dictionary words only, is throttled and cached, and never changes text.
- **M6, Latin into English and Spanish, Ancient Greek, Orbergise.** Latin analysis with constraint-based
  disambiguation, an interlinear view and readable English and Spanish sentences (201 of 201 own sentences in both
  languages after a second analyser pass; a 40-sentence blind batch scored 31 English and 25 Spanish at first run).
  Ancient Greek (Attic) morphology, realiser, checker and transfer from English and Spanish, with Greek into English
  and Spanish (114 of 114 reference lines after two loops; 40 of 40 back). Orbergise rewrites Latin subtitles in
  the vocabulary of the chosen tier with a meaning check (60 of 60 own cases, 20 of 20 blind).
- **M7 (part), the Latin quality loops.** A third loop on English into Latin with a generalisation guard (every rule
  comes with its own test sentences, a 20-sentence blind check scored 5 at first run and 15 after fixes). Tuning
  sample of 100 public-domain cues: 6 → 50 → 71 of 100. Held-out measurement, never read and never tuned on:
  automatic errors 10.8 % → 6.8 % on 74 own cues and 25.1 % → 18.9 % on 700 public-domain cues; no cue the engine
  marked OK was wrong in any run. Library follow-ups: Greek supplement lemmas, Attic cell fixes, Spanish pivot
  glosses so Spanish words unknown to the Latin tables still translate, cleaned headwords.
- **M7 (continued), the acceptance file and the Latinity decision (2026-10-07).** The owner's partial film file
  (82 cues: 31 song lines, 9 quoted narrative, 42 dialogue) was measured at first run and reviewed cue by cue
  against classical norms: 79 % of cues wrong. Two tuning loops on it brought that to 27 % and then 3.7 % (3 of 82,
  a tuned number; no "< 1 %" claim is made), with no wrong cue among those the engine marked OK. Fixes that carried
  over to the never-read held-out files: automatic errors on the 700 public-domain cues fell from 16.7 % to 14.4 %
  with nothing broken. The rules + model combination gave no gain on the file at a thousand times the cost. By the
  owner's decision (D18) medieval and ecclesiastical Latin is accepted behind a toggle that is on by default, in the
  interface and in the engine, with a "late Latin" badge in the Word tab. Greek loops 3 and 4 (light verbs, purpose
  and result clauses, participles, weather verbs, degree words, negation order) kept 114/114 and raised Spanish
  → Greek to 39/40. A first Orbergise run on real material showed meaning-changing word swaps and emoji handling
  bugs; the fix loop is open (ledger C27).
- **Measurement.** `tools/eval` runs every engine combination and fidelity over a file and reports cue-level error
  rates with exact confidence intervals, determinism, export identity and memory, guards the held-out set, builds
  expert review sheets and prepares the Google Translate side-by-side (live run pending).
- **Documentation.** User guides in English and Mexican Spanish, this changelog, the README and the licence.

### Known limitations

- Not yet run or timed on Windows; the owner's smoke-test checklist is in [docs/BUILD.md](docs/BUILD.md).
- The acceptance file and the Google Translate side-by-side are still pending (they need the owner's files and
  machine). On new texts expect about one cue in five to be flagged Fix or wrong on the public-domain register;
  cues marked OK have been right in every measurement so far.
- The local model does not improve Latin (gate failed), so engine ii only helps reading the source language.
- Names are added to the glossary by hand; the engine does not suggest them yet.
