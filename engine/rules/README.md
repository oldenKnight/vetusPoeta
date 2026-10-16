# engine/rules (`vp_rules`) — C1: the Latin side

Rule engine of DESIGN.md §6 and §10. This task delivers the Latin primitives the engine is assembled from
(`makeEngine()`, the frame builder, transfer and cue assembly are task C2). Static library; links `vp_lex`,
`vp_core`. Headers in `include/vp/`:

| Header | What |
|---|---|
| `rules.h` | the engine interface the CLI uses [contract, unchanged] |
| `curated.h` | `CuratedData::load(dir)`: every `data/curated/*.tsv` and `order_la.txt` into sorted tables keyed by `latin_key`; missing file = error with hint, bad line = warning |
| `morph.h` | `analyseLatin` (exact key, enclitics -que/-ne/-ue, u/v i/j folding, capitals, paradigm fallback), `analyseGreek` (exact, then accent-insensitive), `generate` (exact FEAT cell, tolerant cell, participle + `sum` periphrasis, comparison, paradigm fallback with `fromRule`), feature builders, `findLemma`, `parsePrincipal`, `displayForm` |
| `realise_la.h` | `LaClause` / `LaNP` and `LatinRealiser::realise` -> `LaSentence {text, tokens, reasons, flags}`; components `FormSelector`, `Agreement`, `CaseAssigner`, `Orderer`, `Negation`, `Pronouns`, `Names`, `Punctuation`, `Macrons`, `Emoji` |
| `check.h` | `LatinChecker::check(text, options)`: A1 known form, A2 Whitaker (warning), A3 agreement, A4 case government, A6 tier ceiling, from the text alone (hints optional) |

## Design in short
* **Forms** come only from the lexicon (`Lexicon::generate` by FEAT id, else the best cell of the lemma with merged
  genders / degree none = positive, else the perfect participle declined + `sum` for perfect passives and deponents).
  The rule-based paradigm (`src/morph/paradigm_la.cpp`) runs only for lemmas without a table and only for regular
  1st/2nd declension nouns, 1st/2nd class adjectives, 1st/2nd conjugation verbs and -ior/-issimus comparison; every
  such form is `fromRule` (TokenView flag, reason, sentence flag `from-rule`), so the cue can never be OK.
* **Order** is read from `data/curated/order_la.txt`: slot templates (`order.decl`, `order.copula`, `order.exist`,
  `order.inf`, `order.imp.long`), second-position connectors (`order.conn`), adjective exceptions and quantity words
  (`order.adj`), time adverbs (`order.adv`), enclitic cum forms (`order.prep`). Built-in defaults apply when a rule
  is missing (a warning is recorded). Everything else of the file is implemented in code, rule by rule (see
  `docs/rules_la_notes.md` for the implemented / partial / open list).
* **Checker** re-analyses every output word. Agreement is derived from the readings, never from the generator: a
  token that can be a noun is a head; a pure modifier must agree with an adjacent head, be a valid predicate of a
  copula, or stand alone as a substantive (then it may not share its case with a neighbour without agreeing). Subject
  and verb are matched by person and number per clause segment (punctuation, subordinators, relatives split
  segments). A4 uses `preps_en_la.tsv` (preposition -> cases) and `valency_la.tsv` (object case), and the ablative
  of agent with ā/ab for passives.
* Deterministic, allocation-light: the realiser and checker reuse their buffers; RssAnon is flat over 10,000
  realisations (release build asserts <= 5 %).

## Tests
`engine/tests/test_rules_la.cpp` (doctest, in `vp_tests`). Always: curated loaders, fixture lexicon
(`tests/fixtures/lex/latin.vpl`), a mini lexicon built with the reference encoder (paradigm fallback end to end).
With the real library (`VP_LATIN_VPL`, default `data/work/latin.vpl`; skipped with a message otherwise):
61 analyses (`tests/fixtures/rules/analyse_la.tsv`), 114 generated cells (`generate_la.tsv`), 106 realised clauses
in the style of `tests/regression/expected/own_dialogue.la.gold.txt`, 39 checker rows (`check_la.tsv`), 2,000
template clauses with zero A1/A3/A4 faults, 200 deliberate corruptions all caught, determinism, RSS.
```
cmake -S . -B build-rules -DVP_BUILD_GUI=OFF -DVP_WITH_LLM=OFF && cmake --build build-rules -j4
./build-rules/engine/tests/vp_tests -tc='rules-la*' -s | grep MESSAGE
VP_RULES_DEBUG='Puella parvus est.' ./build-rules/engine/tests/vp_tests -tc='rules-la: debug*'   # readings dump
```
