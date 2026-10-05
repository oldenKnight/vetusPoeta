# Decisions approved by the owner (binding)

Date: 2026-10-05. Owner approved the plan with "start"; items marked (F) were delegated to the
main agent to decide.

| # | Topic | Decision |
|---|---|---|
| D1 | App name | "vetus poeta" (lowercase wordmark) |
| D2 | Platform | Windows 10/11 desktop. C++17 engine sidecar + WebView2 shell + ES5 vanilla JS UI. Architecture and conventions follow BabyDaVinci. Linux build + MinGW cross-compile for CI; owner smoke-tests on Windows. |
| D3 | Licence | Proprietary, source visible. No GPL data (Collatinus excluded). Allowed data: Wiktionary via Kaikki/Wiktextract (CC BY-SA 4.0), Perseus LSJ and Lewis & Short (CC BY-SA), Whitaker's Words (permissive). Attribution screen in the app; the library data file carries its licence notice. |
| D4 | Minimum hardware, all three engines | Intel i3 with AVX2 (4th gen+), 4 GB RAM, integrated graphics (unused), ~800 MB disk. App peak RAM < 1.2 GB with the model loaded, < 250 MB without. Model file <= 500 MB. |
| D5 | Local model (engine ii) | 0.5B-class instruct model, 4-bit GGUF, vendored llama.cpp, CPU only, loaded per job and unloaded after. Role: source understanding, sense disambiguation, reranking of rule-engine candidates, vocabulary-constrained decoding. The rule engine owns grammar and style. Its standalone numbers will be reported truthfully even if low. |
| D6 | Google Translate comparison | The main agent runs the same test texts through the Google Translate web page (Latin only; GT has no Ancient Greek) and publishes a side-by-side. |
| D7 | Held-out test (F) | Ask the owner for a second subtitle file. Until then, a held-out set of our own sentences and a public-domain English text (not used for rule tuning) is kept in `tests/heldout/`. |
| D8 | Spanish glosses (F) | English pivot from Wiktionary translations + Spanish Wiktionary extract, plus hand-written and verified es-MX glosses for the tier 1 and tier 2 vocabulary (~3000 words). |
| D9 | Emojis (F) | On in the app view; off in the exported subtitle file unless the user toggles it on in Export. Only unambiguous depictable nouns get an emoji. |
| D10 | Subagents (F) | Max 4 active, depth 2. Implementers: Opus. Pre-plan: Sonnet. Planner/designer/critic: main agent. |
| D11 | Order of work (F) | 1) English/Spanish -> Latin subtitles (acceptance test), 2) Latin -> English/Spanish, 3) Ancient Greek both ways, 4) Orbergise, 5) polish. |
| D12 | Fidelity toggle | Meaning fidelity is never traded. "Extremely faithful" <-> "flexible" moves the vocabulary tier: T1 = Familia Romana core, T2 = T1 + Roma Aeterna + core classical 2000, T3 = full library. Flexible may paraphrase with T1 words. Greek: T1 = Athenaze core, prefer words shared with Modern Greek with unchanged meaning. |
| D13 | Orthography | Latin: Orberg conventions, u/v distinguished, i for consonantal j, macrons available (shown in app; export switch, default off in the file). Greek: Attic, polytonic, final sigma, no macrons. |
| D14 | Error metric | Cue-level: a cue is wrong if it has any grammar, meaning or vocabulary fault. Reported per engine and per combination, on the acceptance file and on the held-out set, with the caveat that the acceptance file was used for rule tuning. |
| D15 | Subtitle formats | .srt first, then .vtt and .ass. Timing, numbering, styling tags byte-identical. Lines re-broken at 42 chars, max 2 lines, reading-speed warning. |
| D16 | Autosave / recovery | Project file is a zip with manifest + cues + glossary + corrections; atomic save; autosave by default; lock file + recovery on crash or corrupted file (BabyDaVinci project module pattern). |
| D17 | In-app languages | en-US and es-MX, JSON string tables, switchable at runtime. |
