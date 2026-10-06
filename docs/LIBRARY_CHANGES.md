# Library changes (B4b, LIB-3)

Built into `data/work/next/` with the B4b pipeline; the live `data/work/*.vpl` were not touched. All counts below are
from that build (stage metas `data/work/next/<lang>/*.json`, `build.py --out data/work/next --report`).
Inputs: the raw dumps of A4/B4 (unchanged) and `data/curated/` **as in the working tree on 2026-10-06** (it contains
uncommitted rows of other tasks: `tiers_la.tsv`, `gloss_es_la.tsv`, `macron_overrides.tsv` (`thea`), ...; the build
reads whatever is there when it runs).

## Swap instructions (main agent)
1. Read the rules-test outcomes below (two Greek assertions change for known reasons; nothing else).
2. Promote the four files:
   ```
   mv data/work/next/latin.vpl data/work/next/greek.vpl data/work/next/english.vpl data/work/next/spanish.vpl data/work/
   ```
3. Optional, so that `build.py --out data/work --check` and later partial builds see the B4b intermediates:
   copy `data/work/next/{la,grc,en,es}/*` over `data/work/{la,grc,en,es}/` and `data/work/next/review_tier_sheet_*.csv`
   to `data/work/`. The next/ tree was pruned to save disk (the machine ran down to 38 MB free during the build):
   intermediates byte-identical to the live ones were deleted (`la/` and `en/`, `es/` kaikki+resolve outputs, the
   translation tables), and `grc/table_forms.tsv` was removed after the last pack; `build.py --next` copies whatever
   a requested stage lacks back from `data/work/<lang>/` (copies, never links), and
   `build.py --raw data/raw --out data/work --next --lang grc --stage kaikki` regenerates the Greek table in 40 s.
4. `tools/build_library/expected_counts.json` already holds the next build's counts (`build.py --out data/work/next
   --check` passes); after step 3 the same check passes on `data/work`.
5. Stale until then: the C9 test assertion "ἀποφεύγω aorist -> missing-form" (the cells now exist) and the A3 false
   positive described under "Rules tests"; both are for C12 / the main agent, not data bugs.

## Sizes
| File | before (B4) | after (B4b) | lemmas before -> after | analyses before -> after |
|---|---|---|---|---|
| latin.vpl | 90,226,096 B (86.0 MiB) | 90,964,592 B (86.8 MiB) | 65,315 -> 65,315 | 1,973,081 -> 1,997,054 |
| greek.vpl | 83,260,032 B (79.4 MiB) | 82,420,496 B (78.6 MiB) | 23,695 -> 23,695 | 1,582,957 -> 1,560,064 |
| english.vpl | 68,611,936 B (65.4 MiB) | **9,320,084 B (8.9 MiB)** | 635,191 -> 68,971 | 1,373,998 -> 280,789 |
| spanish.vpl | 41,212,828 B (39.3 MiB) | **23,776,004 B (22.7 MiB)** | 142,830 -> 66,255 | 1,387,451 -> 854,271 |

SHA-256 (first 16 hex): latin 59f7efd75fc93ccc, greek 48945c3453f95c68, english cbdc3c344c963397, spanish
67081155ee49e66d. Pack peak RSS / wall: la 2.07 GB 77 s, grc 1.63 GB 52 s, en 485 MB 26 s, es 438 MB 19 s. Whole
build: kaikki+resolve 15:34 (peak 1.03 GB), import_aux..pack 6:13 (peak 2.12 GB).

## 1. Greek fixes
| Fix | Where | Count |
|---|---|---|
| (a) particles: head templates `grc-particle` / `grc-part` and Kaikki pos `particle` -> POS particle (12), never participle (13) | `tagmap.pos_name` | 36 lemmas moved participle -> particle: δέ δή μήν μή οὖν τε γε κε οὐ τοι πω μέν ἄν ἐάν μέντοι οὔ ἆρα ἦ μῶν περ μά ... οὐ μή, μή οὐ (γάρ is a conjunction in Wiktionary and stays one). Verified in greek.vpl: οὐ μή οὖν γε ἆρα δέ μέν τε = particle; participle templates `grc-part-1&2` / `-1&3` / `grc-part form` unchanged |
| (b) final sigma: σ at word end (before the end, a space or punctuation; not before an apostrophe) -> ς in every display (cells, head-line forms, headwords, form-page displays); keys unchanged (greek_key folds ς) | `grcfix.final_sigma` in the kaikki stage | 1,828 table cells + 5 head-line forms + 7 headword / form-page displays rewritten. In the files: GENX cells ending in σ 669 (204 lemmas) -> 0; ANAL displays 1,801 -> 0. (C9's 729 counted another subset of the same cells.) |
| (c) article in cells ("τῆς ἀνθρώπου") stripped to the bare form; only "article + one word" (any dialect: τῇσι, τᾶς, ὁ/ἡ ...; ἦ is not taken for ἡ) | `grcfix.strip_article` in the kaikki stage, any POS (suffix tables and mis-tagged participles carry them too) | 2,128 cells of 442 lemmas stripped (C9 saw 165 lemmas among the beginner nouns). GENX cells with an article 1,698 (336 lemmas) -> 0; the multi-word ANAL keys "ἡ κύων" etc. are gone, the bare forms gain those readings |
| romanised cells ("taîs kŭsĭ́(n)", "tēîsĭ thălắssēisĭ") leaking into Greek tables dropped (also 112 mixed garbage cells such as "Κλεῖτoρος" with a Latin o) | `grcfix.latin_script` | 23,118 table cells + 31 head-line forms; GENX 28 -> 0 |
| (d) Epic/Ionic tables without a dialect marker: a nominal table whose genitive singular ends in -οιο or dative plural in -ῃσι(ν)/-ῃς gets "Epic"/"Ionic" prepended to its marker (-> ANAL bit4) | `grcfix.dialect_guess` in the kaikki stage | 1 table in this dump (every other noun/adjective table names its dialect). The visible cause of "unflagged Epic" analyses was the merge rule: a form page that names no dialect cleared bit4 of the Epic table row it merged with. Now such a page does not vote on bit4 (`resolve.FLAG_NODIAL`): table+form-page analyses keeping bit4 3,507 -> 13,468 |
| (e) δεῖ / ἔδει / δεήσει, ἦσθα, βούλει, οἴει via `data/curated/lexicon_overrides_grc.tsv` (key, tags, form, old, note) applied by pack: the GENX cell of the feature word gets the form, the ANAL row is added/merged, `old = dialect` puts bit4 on the replaced form's reading of that cell | `pack.load_grc_overrides`, `apply_cell_overrides` | 7 rows, 8 cells set (βούλει fills middle and passive; δεήσει was already the cell), readings of δέει, δέειν, ἔδεε, ἔδεεν demoted to non-Attic (4). Result: δεῖ: δεῖ / δεῖν / ἔδει / δεήσει; εἰμί impf. 2 sg ἦσθα (Attic and plain cell); βούλομαι pres. 2 sg βούλει; οἴομαι pres. 2 sg (Attic) οἴει |
| πίνω aor. imperative: **no row, πίε stays.** LSJ s.v. πίνω (grc.lsj.perseus-eng17.xml; lsj.tsv keeps only the short glosses, so the full entry was read): "imper. πίε Od. 9.347, Men. 151 ...; also πῖθι Cratin. 141, Ion Trag. 27, Ar. V. 1489, Amips. 18, Antiph. 163.1, etc." Both are attested in Attic (πῖθι in Old Comedy, πίε in Menander); LSJ gives πίε first, so the cell stays as instructed; the engine's own attested override (πῖθι) is unaffected | comment in the override file | - |
| ἀποφεύγω aorist: `@from aorist ἀπο+φεύγω` builds the compound's missing tenses from the simplex table (unmarked/Attic tables only): prefix elided before a vowel (not περι/προ), aspirated before a rough breathing, breathing of the simplex dropped, the recessive accent of a finite disyllable with a short final ε/ο moves onto the prefix (φύγε -> ἀπόφυγε) | `grcfix.compound`, `pack.compound_rows` | 36 cells: ἀπέφυγον ἀπέφυγε ἀπεφύγομεν ..., ἀποφύγω ἀποφύγοιμι, ἀπόφυγε ἀποφυγέτω, ἀποφυγεῖν, ἀποφυγών (cells keep the simplex's length marks: ἀπέφῠγον); ANAL rows added. Other tenses can be added by listing them after `@from` |
| (f) LEMM deponent (bit2): Greek verb whose headword ends in -μαι and that has no present active cell in an Attic or unmarked table; Latin -or verb whose present active 1st singular indicative cells are all -r forms | `pack.deponent_signal` | Greek 253 lemmas (ἔρχομαι, ἐπανέρχομαι, γίγνομαι, μάχομαι, ...); Latin 0 (every Latin -or table already says `deponent`) |

## 2. Latin additions
**Whitaker-only analyses** (`whitaker_gen.py`, ANAL flag bit2): DICTLINE stems + INFLECTS endings for the 22,697
entries joined to a Kaikki lemma (nouns decl. 1-5, adjectives decl. 1-3 positive, verbs conj. 1-4 with deponent,
semi-deponent, impersonal and perfect-only rules; no participles, supines, comparatives, irregular verbs). A row is
added only when **no lemma** of the lexicon has an analysis of that key (a Whitaker reading never adds ambiguity to a
word Kaikki knows: the first version gave "magistrum" a genitive-plural reading of magistra and hid the A4 error of
"Magistrum pāret." in C1's checker table), and, for a lemma with a table, only for a feature word the table fills.
Inflections used: age X/B/C, frequency A/B/C; bit6 (rare) when the ending is not Whitaker's first choice or early, or
the entry archaic; bit5 when the entry is late/medieval. Entries whose forms mostly miss the lemma's table (wrong
join) are rejected.

| Count | |
|---|---|
| ANAL rows added | **24,108** (21,360 keys, 12,363 lemmas; 70 lemmas without a Kaikki table) |
| entries used / rejected (wrong join) / POS mismatch | 22,697 / 721 / 827 |
| forms skipped: feature word not in the lemma's table / key known for another lemma | 28,045 / 6,953 |

All rows are listed in `data/work/next/la/whitaker_only.tsv` (key, lemma id, head, packed, features, flags,
display). Sample (every 1,200th row):

| form | lemma | features | flags |
|---|---|---|---|
| abactorium | abāctor | noun genitive plural | whitaker-only, rare |
| adpendebis | adpendō | verb singular second-person future indicative active | whitaker-only |
| apocularier | apocū̆lō | verb present infinitive passive | whitaker-only, rare |
| aviai | avia | noun genitive singular | whitaker-only, rare |
| capnum | capnos | noun accusative singular | whitaker-only |
| cnisai | cnīsa | noun genitive singular | whitaker-only, late, rare |
| condocefaciamur | condocefaciō | verb plural first-person present subjunctive passive | whitaker-only |
| corrigiai | corrigia | noun genitive singular | whitaker-only, rare |
| desolationium | dēsōlātiō | noun genitive plural | whitaker-only, late, rare |
| diversificaverimus | dīversificō | verb plural first-person future-perfect indicative active | whitaker-only, late |
| exstruier | exstruō | verb present infinitive passive | whitaker-only, rare |
| haustubus | haustus | noun dative plural | whitaker-only, rare |
| insiderit | īnsideō | verb singular third-person future-perfect indicative active | whitaker-only |
| maestarier | maestō | verb present infinitive passive | whitaker-only, rare |
| novercum | noverca | noun genitive plural | whitaker-only, rare |
| percursionium | percursiō | noun genitive plural | whitaker-only, rare |
| praevenirier | praeveniō | verb present infinitive passive | whitaker-only, rare |
| rependerit | rependō | verb singular third-person perfect subjunctive active | whitaker-only |
| sicerum | sīcera | noun genitive plural | whitaker-only, late, rare |
| terminationium | terminātiō | noun genitive plural | whitaker-only, rare |

Most rows are alternative endings Whitaker accepts (gen. pl. -ium/-um, archaic gen. sg. -āī, passive infinitive
-ier, perfect stems of a second spelling); they are flagged and only ever used for analysis (A1/A2), never generated.

**Macron overrides at build time**: `data/curated/macron_overrides.tsv` (narro narr -> nārr; thea the -> thē, the
second row is another task's uncommitted addition) is applied to the GENX cells and ANAL displays of the lemma (2
lemmas, 173 analysis displays; `vpengine inspect latin.vpl narrat` shows nārrat ... nārrandī). The **headword stays
the dictionary's** (narrō): C11's la2x test names the lemma by its head, and the runtime override file stays in use
for safety (decision 5 of rules_la_notes.md).

## 3. English / Spanish size cut
Selection (`morphcut.py`): a single-word lemma is kept when (a) its key or one of its form keys occurs in the UD
treebanks (`data/raw/ud/en_ewt-*.conllu`; `es_ancora-*`, `es_gsd-*`; LEMMA and FORM columns), or (b) it is among the
top 60,000 lemma records by senses + translation-table rows, or (c) its key or a form key is a word of the curated
files the engine reads for that language. ANAL display strings are dropped (display = key; the NLP side never reads
them).

| | English | Spanish |
|---|---|---|
| kept by UD / proxy / curated | 30,340 / 38,134 / 497 | 32,434 / 33,792 / 29 |
| lemmas kept of single-word records | 68,971 of 635,191 | 66,255 of 142,830 |
| size | 68.6 MB -> **9.3 MB** | 41.2 MB -> **23.8 MB** |

Lemma coverage (`tools/build_library/coverage.py`, numbers only; it never prints a word, so it is safe on the
held-out files): tokens whose key, lower-case key or apostrophe/hyphen first part has an analysis.

| File | before: unknown tokens (capitalised) | after: unknown tokens (capitalised) | coverage after |
|---|---|---|---|
| tests/regression/own_dialogue.en.txt (512 tokens, 239 types) | 0 | 0 | 100.00 % |
| tests/heldout/own_heldout.en.srt (317 tokens) | 0 | 0 | 100.00 % |
| tests/heldout/oz_dialogue.en.srt (7,726 tokens, 1,048 types) | 6 (6) | 30 (19) | 99.61 % (types 98.47 %) |

The English file has room under the 25 MB target. Measured in memory (no files written) with a larger proxy
cut-off: 120,000 -> 16.0 MB, Oz unknown 20 (13 capitalised); 200,000 -> 24.1 MB, Oz unknown 10 (all capitalised,
i.e. names). If the main agent prefers coverage over size, set `morphcut.TOP_N = 200000` and rerun
`--next --lang en --stage pack` (Spanish is at 23.8 MB with 60,000 and should stay there).

## 4. Greek gloss_es
Rule (gloss stage, Greek only): an es.wiktionary gloss that shares no word with the Spanish translations of the words
of the English first sense (stop words included: "from", "other") belongs to another sense; the EN->ES pivot of sense
0 replaces it (head words after a colon when the gloss has one; when a head has several Spanish translations, the one
that also occurs in the lemma's own es.wiktionary senses wins). When the pivot finds nothing, the es.wiktionary gloss
stays. Counts: 178 es.wiktionary glosses kept, 52 replaced, 5 kept for lack of a pivot; gloss_es coverage 20,337 ->
20,341 lemmas. The colon rule also changes some pivot glosses (ὡς, ὅτι, σύ, ὅστις).

DCC Greek core, ranks 1-50 (* = changed):

| DCC rank | lemma | gloss_en | gloss_es before (source) | gloss_es after (source) |
|---|---|---|---|---|
| 1 | ὁ | the | el, artículo determinado masculino singular (eswikt) | el, artículo determinado masculino singular (eswikt) |
| 2 | αὐτός | self | ser (pivot) | ser (pivot) |
| 3 | καί | and | y (pivot) | y (pivot) |
| 4 | δέ | but, and | mas (pivot) | mas (pivot) |
| 5 | τῐ́ς | who? | quien (pivot) | quien (pivot) |
| 6 | εἰμῐ́ | to happen | acaecer (pivot) | acaecer (pivot) |
| 7 | οὗτος | here | acá (pivot) | acá (pivot) |
| 8 | ἤ | or | o (pivot) | o (pivot) |
| 9 | ἐν | in; on; at; among | entre (pivot) | entre (pivot) |
| 10 | μέν | on the one hand, while, whereas | uno, considerando (pivot) | uno, considerando (pivot) |
| 11 | τῐς | someone; anyone; a certain one | se (pivot) | se (pivot) |
| 12 | ὅς | this | aquesta (pivot) | aquesta (pivot) |
| 13 | γᾰ́ρ | for | como (pivot) | como (pivot) |
| 14 | οὐ | not | no (pivot) | no (pivot) |
| 15 | λέγω | to put in order, arrange, gather | colocar, acotejar, allegar (pivot) | colocar, acotejar, allegar (pivot) |
| 16 | ὡς | introducing a clause expressing a fact: that | introducir (pivot) | para que (pivot) * |
| 17 | τε | and | y (pivot) | y (pivot) |
| 18 | εἰς | into | adentro de (pivot) | adentro de (pivot) |
| 19 | ἐπῐ́ | on, upon | en (pivot) | en (pivot) |
| 20 | κᾰτᾰ́ | against; opposing | enfrentado (pivot) | enfrentado (pivot) |
| 21 | ἐγώ | first person singular personal pronoun: I, me, my | yo (eswikt) | yo (eswikt) |
| 22 | πρός | before, in presence of, in the eyes of, in the sight of | presencia, ojo, apuntar (pivot) | presencia, ojo, apuntar (pivot) |
| 23 | γῐ́γνομαι | to be born | nacido (pivot) | nacido (pivot) |
| 24 | ἐᾱ́ν | if | si (pivot) | si (pivot) |
| 25 | δῐᾰ́ | in a line | línea (pivot) | línea (pivot) |
| 26 | ᾰ̓λλᾰ́ | but | mas (pivot) | mas (pivot) |
| 27 | πᾶς | all; every; each | toda (pivot) | toda (pivot) |
| 28 | ἔχω | to have, possess, contain, own | poseer, contener (pivot) | poseer, contener (pivot) |
| 29 | ἐκ | Out of, from | de, desde (eswikt) | de, desde (eswikt) |
| 30 | πολῠ́ς | large, great | grande, groso (pivot) | grande, groso (pivot) |
| 31 | περῐ́ | [with genitive] |  () |  () |
| 32 | μή | not | no (pivot) | no (pivot) |
| 33 | ὅστῐς | indefinite relative pronoun: whoever, whichever, whatever | indefinido, cualesquiera, cual sea (pivot) | cualesquiera, cual sea, cualquiera (pivot) * |
| 34 | ᾰ̓́ν | in that case and future tense | empaquetar (pivot) | caja (pivot) * |
| 35 | σῠ́ | second person singular personal pronoun: thou, you | segundo, tú (pivot) | tú (pivot) * |
| 36 | ᾰ̓νᾰ́ | on board | abordar (pivot) | abordar (pivot) |
| 37 | ὅτῐ | after verbs of perception and emotion | verbo, introducir (pivot) | para que (pivot) * |
| 38 | εἰ | if | si (pivot) | si (pivot) |
| 39 | ᾰ̓́λλος | the other, all others, all besides, the rest | otro (eswikt) | otro (eswikt) |
| 40 | ᾰ̓πό | from, away from | fuera (pivot) | fuera (pivot) |
| 41 | φημῐ́ | to think | pensar (pivot) | pensar (pivot) |
| 42 | ῠ̔πό | from underneath | bajo (pivot) | bajo (pivot) |
| 43 | ποιέω | to make | hacer (pivot) | hacer (pivot) |
| 44 | οὖν | then | de entonces (pivot) | a continuación (pivot) * |
| 45 | λόγος | that which is said: word, sentence, speech, story, debate | cálculo, cómputo (eswikt) | palabra, sentencia, discurso (pivot) * |
| 46 | πᾰρᾰ́ | from | de (pivot) | de (pivot) |
| 47 | οὕτως | in this manner, thus, so | manera (pivot) | manera (pivot) |
| 48 | πρότερος | before, in front | frente (pivot) | frente (pivot) |
| 49 | θεός | a deity; a god | dios, deidad, divinidad (eswikt) | dios, deidad, divinidad (eswikt) |
| 50 | μετᾰ́ | in the midst of, among, between, with | entre (pivot) | entre (pivot) |

7 of 50 changed: 4 better (λόγος, σύ, ὡς, ὅτι), 2 neutral (ὅστις, οὖν), 1 still wrong in another way (ἄν: no pivot
fits a modal particle; "caja" instead of "empaquetar"). The remaining poor pivots (αὐτός "ser", εἰμί "acaecer",
οὗτος "acá", λέγω "colocar, acotejar, allegar", διά "línea", ἀνά "abordar") come from Wiktionary's first English
sense and need hand-written es-MX glosses (a `gloss_es_grc.tsv` like the Latin one; not in this task).

## 5. Teacher review sheets
`data/work/next/review_tier_sheet_la.csv` (3,981 rows) and `review_tier_sheet_grc.csv` (614 rows): key, head, pos,
tier, source (teacher | curated | dcc+el | dcc | whitaker | candidate), dcc_rank, whitaker_code (best Whitaker
frequency letter), gloss_en, gloss_es, lemma_id; tiers 1-2 plus Latin tier-3 candidates with Whitaker frequency A/B.
How to edit the tier files: `docs/TEACHER_REVIEW.md`. Build change found on the way: rows with tier 2 in
`tiers_la.tsv` / `tiers_grc.tsv` were ignored at build time (only tier 1 was read); they now give tier 2 (the engine
already honoured them at run time). Tiers now: la T1 511 / T2 3,256 / T3 59,312 / T0 2,236; grc T1 389 / T2 225 /
T3 22,963 / T0 118 (Greek T1 grew by C9's 211 derived rows in `tiers_grc.tsv`).

## 6. Verification
* Python: 87 unittests green (`python3 -m unittest discover -s tools/build_library/tests`): 64 earlier + 23 in
  `tests/test_b4b.py` (particle POS, final sigma, article stripping, romanised cells, dialect guess, compound accents,
  merge rule, a kaikki+resolve+pack run over the hand-made `tests/fixtures/kaikki/grc_b4b.jsonl` checking cells,
  overrides, compound, deponent flag and a valid encoded file, Latin deponent signal, Whitaker generator incl. guards
  and deponent/impersonal rules, macron overrides, size-cut selection and display = key, Greek gloss_es recheck,
  curated tier 2 + teacher sheet, `--next` / `--out-vpl` never writing the live file). `make_vpl_spec.py --check`
  (fixture latin.vpl unchanged), `compare_spec.py` (B1's three reference files byte for byte),
  `make_golden.py --check` OK.
* `build.py --out data/work/next --check`: OK against the rewritten `expected_counts.json` (before the rewrite it
  flagged exactly the expected moves: en/es pack size/lemmas/analyses, grc gloss_es_eswikt, grc tier1/tier2 and la
  gloss_es_curated from the curated files).
* All four files: `vpl_inspect.py --validate --sha` OK; `vpengine inspect` (build-lib3, Release) opens each one
  (la 1,101,579 keys / grc 936,603 / en 149,268 / es 535,782).
* Rules tests (`vp_tests -tc='*rules*'`, same binary, live files vs `VP_LATIN_VPL`, `VP_GREEK_VPL`,
  `VP_DATA_WORK=data/work/next` with `next/nlp -> ../nlp`):
  * rules-la: all green with both (checker table, 2,000 clauses, generate/analyse tables). The "thither" tier-note
    check passes with both: it reads `data/curated/tiers_la.tsv`, not the lexicon.
  * rules-en: regression own_dialogue 114/114 normalised matches with both; "cues with T1/T2 candidates for every
    content word" 103 -> 110 (the curated tier-2 rows now reach the lexicon).
  * rules-grc: gold 40/40, analyses/generate tables green with both. Two assertions change:
    (1) `test_rules_grc.cpp:870` expects `missing-form` for ἀποφεύγω aorist: the cell now exists (the fix; the
    assertion should become "ἀπέφυγε"); (2) 2,000 generated clauses: 10 A3 faults, all "ὁ παῖς ὃν ἔχεις τρέχει":
    ἔχεις is now also nominative/accusative plural of ἔχις "viper" (true; before, those cells were keyed
    "αἱ ἔχεις" with the article) and the checker takes it for the main clause's subject. The checker should prefer the
    finite reading of a word right after a relative pronoun (C12). 200/200 corruptions caught, 0 accent warnings.
    The particle cases do not change outcome: C9 already treated pos 13 without case/mood as a particle.
  * rules-la2x (another task, in progress): the same 24 assertions fail with the live and the new files (own
    sentences, disambiguation, engine pairs, A9).
  * Final run (10:46, after the Spanish repack): "rules-c: curated loaders take the new tables and odd rows" crashed
    with SIGSEGV **with the live files as well** (it passed at 10:37 with both; other tasks were editing
    `data/curated` and `engine/rules` meanwhile and build-lib3 was compiled before). Excluding that case
    (`-tce='rules-c: curated loaders*'`): live 57/61 cases pass, next 55/61, the difference being exactly the two Greek
    assertions above.
* Not run: sanitizer / MinGW builds (no C++ was changed), the full ctest (other implementers' uncommitted engine/cli
  changes are in the tree).

## Pipeline changes (for maintainers)
`build.py --next` / `--out-vpl`, `grcfix.py`, `whitaker_gen.py`, `morphcut.py`, `coverage.py` (new); `tagmap.py`
(particles), `kaikki.py` (Greek clean-up), `resolve.py` (FLAG_NODIAL), `gloss.py` (Greek gloss_es), `tiers.py` (tier-2
rows, teacher sheet), `pack.py` (overrides, compounds, deponent, macrons, Whitaker rows, size cut). The gloss stage
now declares `en/lemma_index.tsv` and `es/lemma_index.tsv` as inputs (its lemmatiser always read them; without them
it silently falls back to identity and the glosses change). Details: `tools/build_library/README.md`, "B4b".
