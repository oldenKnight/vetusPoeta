# Greek engine path (C12) — coverage, mismatches, gaps, questions

Code: `engine/rules/{include/vp/transfer_grc.h, include/vp/grc2x.h, src/transfer_grc, src/grc2x, src/engine_grc}`, one
delimited block "C12 grc" in `src/engine/engine.cpp` (three one-line hooks: translate, check, inspect); tests:
`engine/tests/test_rules_grc2.cpp`; fixtures `tests/fixtures/grc2x/sentences.tsv`. Reports (written by the tests):
`<build>/regression_report_grc.txt`, `regression_report_grc_es.txt`, `grc2x_report.txt`.

## What is implemented
* **A3 fix (C9 checker).** A form with a finite-verb reading and a noun reading and no article of its own (ἔχεις =
  ἔχω 2 sg / ἔχις nom-acc pl) is no longer reported as a disagreeing subject: the verb reading gives a consistent
  clause. C9 generator again 2,000 clauses / 0 faults; ἀποφεύγω aorist test now expects "ἀπέφυγεν." (cells exist);
  the missing-form test uses a missing verb instead. A1b now checks capitalised sentence starts on the lower-case word.
* **Decision 4 in the realiser:** ἆρα + verb first ("ἆρα εἶδες τὴν γαλῆν μου;"); order_grc.txt order.yn is a slot
  template now; C9's table row "ἆρα φιλεῖ ἡ κόρη τὴν μητέρα;" updated.
* **Transfer EN/ES -> Greek** (`GreekTransfer`): REVX on greek.vpl (English keyword; Spanish `es:` keyword, the
  English pivot through spanish.vpl's gloss when it has one); scoring as §10.2 plus Greek tiers (tiers_grc.tsv wins),
  shared_el bonus (+0.05 at fidelity 2, +0.1 at 3, D12), −0.2 for a declinable lemma without a table; teacher glosses
  from the tiers_grc.tsv notes (33 notes added for beginner words) and from readable_grc.tsv read backwards (English
  and Spanish columns: the Spanish teacher index); tier preference and low-tier flag as Latin. Closed classes in code:
  article by the frame's definiteness + possessor + generic plural subject (τὰ ἄνθη, αἱ σφαῖραι), names with the
  article (decision 6), unique definite predicates `forceArticle` (decision 9), ἐγώ/σύ dropped unless emphatic,
  οὗτος/ἐκεῖνος, πᾶς/οὐδείς/τις, numerals 1-12/20/100/1000, ordinals 1-12, prepositions (preps_en_grc.tsv + rules for
  to/for/with/at/in/on/into/of/by/from), particles from connectors (and καί, but ἀλλά/δέ, so/then οὖν, or ἤ,
  because ὅτι), "of course" + clause -> δήπου (phrasebook note), elliptical "This one does." -> αὕτη δὲ τρέχει.
  Tense: aorist for past events, imperfect for past states / progressive / habitual, perfect only for a passive
  resulting state, future; deliberative -> aorist subjunctive; no optative (conditionals -> indicative). Imperatives:
  aorist for a single act, present for the verbs of lexical_en_grc.tsv kind `durative` (the Greek verb decides) and
  for prohibitions (μή + present imperative, decision 8). New table `data/curated/lexical_en_grc.tsv` (phrasal,
  verb+preposition, state adjectives, realia, fixed verbs, durative verbs; English and Spanish rows).
  Realia (decision 1): tea/coffee -> ποτόν with reason "no Attic word; equivalent used" and Check. "Please"
  (decision 2): ἀντιβολῶ at fidelity 1-2, left out at 3. Emoji from emoji_grc.tsv (realiser). Capitals: sentence
  start capitalised (decision 5, `capitaliseGreek`); the regression compares case-insensitively (the gold is lower case).
* **Phrasebooks:** the frame builder runs on a copy of CuratedData whose phrasebooks are phrasebook_en_grc.tsv /
  phrasebook_es_grc.tsv (new, 64 rows mirroring phrasebook_es_la.tsv). Rows added to the English one: "i am late",
  "play cards" (register vp: the last word is the verb, the others are analysed into obliques), "off with {NP}".
* **Cue assembly:** cue/ splitting reused; line breaks through `subs::breakLines` with `greekBreakHints()` (καί ἀλλά
  ὅτι εἰ ἐάν ἐπεί ὅτε ἵνα ὥστε ἐν εἰς ἐκ ἐξ πρός ἀπό μετά διά περί ὑπό παρά ἐπί ἤ οὐδέ); `?` -> `;`, source
  `;`/`:` -> `·`; movable ν across phrasebook pieces. `toMonotonic(text)` (public, transfer_grc.h) for the CLI's
  export option greek:"monotonic": acute from any accent (one per word), breathings / iota subscript / length marks
  dropped, diaeresis kept, monosyllables unaccented except ἤ ποῦ ποῖ πῶς πῇ τίς τί.
* **Checks and confidence:** GreekChecker A1/A1b/A3/A4/A6 + A5/A7/A8 as Latin; A9 is reported "not run for Greek".
  Fix: A1/A3/A4 faults, unknown words, markup; Check: A1/A1b warnings (accent differs), A5 tag position, A6, A7, A8,
  margin < 0.15, realia, name guessed/kept, paradigm forms, missing forms, frame fallbacks, low tier, songs, nonverbal.
  Alternatives: the second candidate of the most ambiguous word; the other speaker gender when unknown.
* **Greek -> EN/ES (`grc2x`):** tokens incl. `;` `·` and elided words; readings from vp::grc::analyse + closed
  tables (article / pronoun tables win; τίς vs τις by the accent) + names_grc.tsv forms + glossary; disambiguation by
  coordinate ascent over local constraints (article agreement ±3/−2, adjective/numeral ↔ noun, preposition case,
  one finite verb per clause, verb ↔ nominative number/person with the neuter-plural rule, ὦ + vocative, infinitive
  after a modal); interlinear words (lemma, features in words, gloss + pivot flag, alternatives, why); a SemFrame per
  clause (predicate position adjectives, neuter nom/acc as object when no 3rd-person subject fits, genitive
  attributes vs genitive objects of ἀκούω etc., δεῖ/ἔξεστι persons, existential ἔστι, subordinates ὅτι/ἐπεί/εἰ/ἵνα,
  particles -> "and/for/so/but"); English (do-support, inversion, tense reversed: aorist -> past, imperfect -> "was
  ...ing", perfect -> present perfect) and es-MX Spanish (pretérito/imperfecto, ser/estar, clitics, personal "a",
  pro-drop, ¿?/¡!). Glosses: new `data/curated/readable_grc.tsv` (165 rows) first, then the lexicon. English /
  Spanish inflection reuses C11's helpers (`la2x/internal.h`: en::verb, en::plural, es::verb, es::adjective ...).

## Results (fidelity 2, speaker f, real data)
| Set | Result |
|---|---|
| EN -> GRC, own_dialogue.en.srt 1-40 vs own_dialogue.grc.gold.txt | **40 / 40** (normalised; exact incl. case/punct 0 / 40 because of decision 5 capitals); ok 30 / check 10 / fix 0 |
| ES -> GRC, own_dialogue.es.srt 1-40 vs the same gold (report only) | **36 / 40**; ok 33 / check 7 / fix 0 |
| GRC -> EN / GRC -> ES, 40 own Attic sentences | **40 / 40** and **40 / 40** |
| Monotonic table | 23 / 23 |
| Determinism | two engines byte-identical on the 40 cues |
| RSS | RssAnon flat over 1,000 cues in batches of 20 (4,724 kB -> 4,724 kB) |
| Latin regression / Spanish regression / Greek primitives | 114 / 114, 100 / 100, all C9 cases green |

Caveat: the 40 gold lines were used for tuning (curated rows, teacher glosses), and the 40 own Attic sentences were
written together with the readable rules; neither is a held-out measure.

## Mismatches (ES -> GRC; EN -> GRC has none)
| # | source | gold | ours | why |
|---|---|---|---|---|
| 24 | Son todos muy groseros. | πάντες πάνυ ἄγροικοί ἐστε | …εἰσιν | "son" is 3rd plural (ustedes / ellos): no 2nd person to read |
| 27 | ¡Que le corten la cabeza! | …τὴν κεφαλὴν αὐτοῦ | …τὴν κεφαλήν | the dative "le" (possessor) is not carried into the slot |
| 30 | Nunca he jugado. | οὐδέποτε πρότερον ἔπαισα | οὐδέποτε ἔπαισα | the Spanish has no "before" (proposed gold alternative) |
| 37 | No hay té. | οὐκ ἔστι ποτόν | ἔστιν οὐδὲν ποτόν | correct Attic too (proposed gold alternative) |

## Gaps seen on lines 41-80 (not tuned; `VP_GRC2_TRY=<file>` prints them)
* Phrasebook gaps: "here you are", "you're welcome", "too late", "it doesn't matter {WH}", "that depends on {WH}",
  "do you know how to {VP}", titles ("Your Majesty" stays in Latin letters, name-guessed, Check).
* Indirect questions after "depends on" / "care where" come out word by word (Check).
* Lexical choices from REVX without a teacher gloss are sometimes odd (pass -> κρίνω, paint -> ζωγράφος); every such
  choice is visible as a reason with its candidates; low margins give Check.
* "once upon a time" -> ἐπὶ χρόνου (literal); a phrasebook row ποτέ would be better.
* GRC -> EN: participles (none in the beginner style), the optative, the dual and crasis are not read as such;
  relative clauses are realised flat; ἄν is ignored.

## Questions for the main agent
1. A9 for Greek: run grc2x on the Greek output and compare source lemmas (as la2x does for Latin)? Implemented hook
   only reports "not run"; the data is there (grc2x readable glosses).
2. Gold alternatives proposed: line 30 "οὐδέποτε ἔπαισα" (for the Spanish source), line 37 "ἔστιν οὐδὲν ποτόν".
3. Should "Of course ..." stay δήπου (beginner tier 2 added to tiers_grc.tsv) or πάνυ γε + clause?
4. Monotonic export: monosyllables lose the accent (modern convention); confirm, or keep every accent as tonos.
5. Spanish personal "a" before specific animals ("¿Viste al gato?", "desata a los bueyes"): keep?

## API changes (recorded in STATUS)
Additive only: `curated::CuratedData::replacePhrasebooks(en, es)` (a copy with the Greek phrasebooks for the frame
builder). New public headers `vp/transfer_grc.h` (GreekTables, GreekTransfer, toMonotonic, capitaliseGreek,
greekBreakHints) and `vp/grc2x.h` (Translator, splitSentences). Internal `src/engine_grc/engine_grc.h` (GreekPath).
Wish for the CLI: call `vp::grc::toMonotonic` on Greek cue text when exporting with greek:"monotonic".


## Decisions by the main agent (2026-10-06) on C12's questions
1. A9 for Greek output uses the Greek -> English path (grc2x) exactly as la2x serves Latin (task C16).
2. The monotonic export's monosyllable rule (accent dropped on one-syllable words except ἤ and the question words)
   is accepted: it is the standard modern convention.
3. δήπου for "of course" + clause stays, tier 2.
4. The Spanish personal "a" before specific animals is kept ("¿Viste al gato?").
Gold: "οὐδέποτε ἔπαισα" and "ἔστιν οὐδὲν ποτόν" accepted as alternatives in the Greek gold.


## Quality loop 2 (C16, 2026-10-06)
Material: regression lines 41-114 of `own_dialogue.en.srt` against the main agent's Attic gold (lines 41-114 were not
tuned by C12); measured with `engine/tests/test_rules_grc2.cpp` (now all 114 lines, report
`<build>/regression_report_grc.txt`, fidelity 2, speaker f, real data). These lines were the tuning set of this loop,
so the numbers below are not a held-out measure. tests/heldout/ was not opened.

### Match-rate log
| step | 1-40 | 41-114 | all | ok / check / fix (all 114) |
|---|---|---|---|---|
| start (HEAD f16325e + the 114-line harness) | 40 / 40 | 14 / 74 | 54 / 114 | 47 / 64 / 3 |
| group 1: phrasebook, lexical, tier rows; transfer (nonfinite / sub verb rows, χρή, elliptical "can", state verbs in the past, pluperfect, perfect active, ἄν, ἐάν + aorist subjunctive, πρίν + infinitive, acc + inf after think, "only if", weekdays, "things", adjectives before an indefinite noun, fixed PPs, "first ... then"); realiser (adjFirst / genFirst, ἄν, verbFirst, εἰ δὲ μή, order.wh.cop, order.yn.inf, imperative last after a fronted sequence adverb, adjective adverbs, indeclinable numerals, χρή by valency) | 40 / 40 | 59 / 74 | 99 / 114 | 77 / 36 / 1 |
| group 2: Attic augments (ηὗρον, ἐβουλόμην, ᾤμην), {WH} slots as dependent clauses, `{1:inf}` slot, ποτε second position, "or" + counterfactual, enclitic pronoun with the modal in yes/no questions, "what day" -> τίς, head-word weighting, adverb after a quantifier subject, "your majesty" by the addressee, see-you order | 40 / 40 | 71 / 74 | 111 / 114 | 82 / 32 / 0 |
| group 3: taught nouns over substantive adjectives, checker A3 adverb cells, A9 round trip wired, emoji rows | 40 / 40 | **71 / 74** | **111 / 114** | **81 / 33 / 0** |

Exact (case and punctuation too): 2 / 114 (decision 5 capitals; the gold is lower case). ES -> GRC (lines 1-40,
report only): 36 / 40 -> 38 / 40. Latin: EN 114 / 114, ES 100 / 100 (unchanged). GRC -> EN / ES 40 / 40, 40 / 40;
monotonic 23 / 23; C9 primitives all green (the C9 gold test now reads the first 40 lines of the longer gold file:
`REQUIRE(gold.size() >= 40)`, no expectation changed). New unit case "C16 constructions": 37 / 37 own sentences that
are not regression lines, plus the weekday alternative, the "your majesty" policy and A9.

### What changed
* **Data** (all teacher-editable, our own rows): `phrasebook_en_grc.tsv` +28 rows (here you are, you're welcome, too
  late, here she is, a little, once upon a time -> particle ποτε, you must be -> ἀνάγκη, I don't (much) care (where),
  that depends on {WH}, it doesn't matter ({WH}), do you know how to {VP}, pass / hand me {NP}, your majesty / highness,
  good afternoon, good night, sleep well, every morning / day, it is raining, it's going to rain), "my name is {NAME}"
  -> "{1} ὄνομά μοι", "see you ..." with the enclitic σε second; `phrasebook_es_grc.tsv` mirrors (+12);
  `lexical_en_grc.tsv` +87 rows and three new kinds: `perfect` (κατάγνυμι: "is broken" -> κατέαγεν), `pp` (fixed
  prepositional phrases: in Latin / Greek -> Ῥωμαϊστί / Ἑλληνιστί, at the bottom -> ἐν τῷ βάθει, to school -> πρὸς τὸν
  διδάσκαλον, by mistake -> ἁμαρτών agreeing with the subject), `weekday` (god's name in the genitive + ἡμέρα, the
  ordinal from Sunday as the alternative); verb-row frames `nonfinite` (go -> εἶμι for infinitives, subjunctives,
  futures and dependent clauses: ἰέναι, ἴωμεν, εἶ) and `sub` (come -> ἥκω in a dependent clause); state rows mad /
  cold / wrong / kind; durative rows (help, write, smile, close, light, believe, think ...; a state verb in the past is
  imperfect when durative, aorist otherwise: ὠργίζετο / ἥμαρτες); `tiers_grc.tsv` +64 rows / 4 notes (teacher glosses:
  house -> οἰκία, story -> μῦθος, letter -> ἐπιστολή, coat -> ἱμάτιον, window -> θυρίς, candle -> λύχνος, clock ->
  ὡρολόγιον, paint -> χρῶμα, breakfast -> ἄριστον, homework -> ἔργον, teacher -> διδάσκαλος, dark -> σκότος / σκοτεινός,
  date -> ἡμέρα ...); `valency_grc.tsv` πιστεύω dat;acc (a thing believed in the accusative), οἴομαι acc+inf, χρή
  impers:acc+inf; `order_grc.txt` 8 new rules (order.yn.inf, order.wh.cop, order.prin, order.acc.inf.think, mood.an,
  order.sub.otherwise, order.cond.aspect, order.adv.subj) and amended order.conn.second (ποτε; "διὰ τί" as a unit),
  order.adj (ordinals and ἕκαστος before the noun; the EN/ES transfer puts adjectives before an indefinite noun except
  for the subject of an existential clause), order.imp; `emoji_grc.tsv` +4 (θυρίς, λύχνος, ἱμάτιον, χρῶμα).
* **Transfer** (`transfer_grc.cpp`): head-word weighting of the sense gloss as the Latin transfer (+0.05 when the
  source word heads the first gloss item, -0.3 when it is only a modifier, not for a teacher's gloss) - house -> οἰκία
  over οἶκος; a taught noun beats an adjective used as a noun (-0.4); "pass me" now goes through the phrasebook
  (δός μοι), "paint" (noun) through the teacher's gloss (χρῶμα), so κρίνω / ζωγράφος are gone; Should -> χρή; elliptical
  "can" -> δύναμαι alone; past perfect -> pluperfect; "would" in a main clause -> ἄν + imperfect / aorist; ἐάν +
  aorist subjunctive for a single event; "before" -> πρίν + infinitive with its own subject (accusative, after the
  verb); verbs with an acc+inf valency take accusative + infinitive, others ὅτι; "only if X" (no main clause) -> εἰ μὴ
  X; "or" + counterfactual -> εἰ δὲ μή, + ἄν clause; elliptical "it is" -> existential ἔστι; "what day" -> τίς ἡμέρα
  (no second τί); "red ones" takes the gender of the noun before the clause and no article unless the source has one;
  adjective adverbs (πρῶτον, ἡδέως from the adjective's adverb cell); "first ... then" -> πρῶτον ... ἔπειτα (front);
  an adverb right after a noun / quantifier subject stays with it ("πάντες ἐνθάδε").
* **Realiser** (`realiser_grc.cpp`, additive fields): GrcNP adjFirst / genFirst, GrcOblique end, GrcSub otherwise,
  GrcClause an / verbFirst; χρή and any impersonal acc+inf modal of valency_grc.tsv like δεῖ; the realiser's own
  defaults (C9 table) are unchanged.
* **Morphology** (`forms_grc.cpp`): attested Attic overrides εὑρίσκω aorist ηὗρον ... (movable ν on ηὗρε), βούλομαι
  imperfect ἐβουλόμην ..., οἴομαι imperfect 1 sg ᾤμην (the Attic-flagged cell ᾤομην is a table error).
* **Checker** (`check_grc.cpp`): an adjective form that is also an adverb cell (πρῶτον, πολύ, ὀλίγον) is not an
  agreement fault next to a noun of another gender ("πρῶτον τὴν θύραν ἄνοιξον"); the 200 / 200 corruptions are still
  caught.
* **Engine** (`engine_grc.cpp`): {WH} slots are translated as dependent clauses; `{1:inf}` renders a {VP} slot as an
  infinitive; "your majesty" picks ὦ βασιλεῦ / ὦ βασίλεια by the addressee: a glossary entry with a gender, else the
  last king / queen named in the file, else masculine with flag addressee-guess (Check); the weekday ordinal is offered
  as an alternative; **A9** (decision 1): `grc2x::Translator::roundTripOverlap` (the source's content lemmas against the
  glosses of the Greek readings: readable_grc.tsv, tier notes, lexicon senses and keywords, names) < 0.5 -> Check.
  Effect on confidence: A9 fails on 2 of 114 cues (lines 36-37, ποτόν for "tea", already Check by decision 1), so the
  confidence counts do not move on this file (a unit case checks a pass at 1.0 and a failing overlap). check() of an
  edited cue reports A9 as not run (no source lemmas), as for Latin.

### Remaining mismatches (fidelity 2)
| # | source | gold | ours | why |
|---|---|---|---|---|
| 52 | The cat could smile. | ἡ γαλῆ μειδιᾶν ἐδύνατο / ἡ γαλῆ μειδιᾶν οἵα τ' ἦν | Ἡ γαλῆ ἐδύνατο μειδιᾶν. | order.inf puts the modal before the infinitive (gold lines 13, 19) |
| 107 | Are you afraid of the dark? | ἆρα φοβῇ τὸ σκότος; | Ἆρα φοβεῖ τὸν σκότον; | σκότος is masculine and neuter in the lexicon (gender MN, both tables Attic, the cells carry no gender); the table's contracted 2 sg middle is φοβεῖ (φοβῇ is not a cell) |
| 112 | You are very kind. | εὔνους εἶ μάλα / μάλα χρηστὸς εἶ / μάλα χρηστὴ εἶ | Πάνυ χρηστὸς εἶ. | "very" is πάνυ everywhere else in the gold (9, 24); εὔνους is not in greek.vpl |

### Proposed gold alternatives (the gold file is unchanged)
- #52 add "ἡ γαλῆ ἐδύνατο μειδιᾶν." - modal before its infinitive as in lines 13 ("οὐ δύναμαι διελθεῖν") and 19
  ("τὰ ἄνθη οὐ δύναται λαλεῖν").
- #107 add "ἆρα φοβεῖ τὸν σκότον;" - ὁ σκότος is the older Attic gender (LSJ: "σκότος, ὁ ... later also τό"), and the
  -ει ending of the 2nd singular middle is regular Attic prose (as βούλει, οἴει, which the engine already uses).
- #112 add "πάνυ χρηστὸς εἶ." - the gold renders "very" with πάνυ in lines 9 and 24; μάλα here alone looks arbitrary.

### Lexicon gaps found (for LIB)
1. No διδασκαλεῖον (school), λάθος (mistake), εὔνους (kind) in greek.vpl: "to school" -> πρὸς τὸν διδάσκαλον (gold
   line 87 alternative), "by mistake" -> ἁμαρτόντες, "kind" -> χρηστός.
2. οἴομαι: the Attic-flagged imperfect 1 sg cell is ᾤομην (should be ᾤμην); βούλομαι: the Attic-flagged imperfect cells
   have ἠβουλ- (the classical ἐβουλ- is unflagged); εὑρίσκω: εὗρον first (classical Attic ηὗρον). Overridden in
   forms_grc.cpp (attested forms only).
3. σκότος has gender MN with masculine and neuter tables mixed and no gender on the cells.
4. φοβέω's table has no contracted 2 sg middle in -ῃ (φοβῇ is only an analysis).
5. Ἑλληνιστί has a capitalised head (ῥωμαϊστί does not).

### Questions for the main agent
1. Adjectives before an indefinite noun as the EN/ES transfer default (gold 48, 64, 66, 86, 81) while the realiser's
   own C9 default stays after the noun; existential subjects after ("ἦν ποτε κόρη μικρά"). Keep?
2. "Your Majesty" without a king / queen in the file and no glossary entry: masculine + Check. Or ask the user via
   the glossary?
3. Weekdays: planetary genitive by default (Ἄρεως ἡμέρα), the ordinal as an alternative. The cue carries a flag
   "weekday" (informational); fine for the UI?
4. Present imperatives for close / light (κλεῖε, ἅπτε, as the gold) are data rows of kind durative; an aorist for a
   single act would follow decision 8 more strictly (κλεῖσον, ἅψον). Keep the gold's practice?
5. "every morning" -> ἑκάστης ἡμέρας ἕωθεν (genitive of time) because the main gold line needs διδασκαλεῖον, which the
   lexicon lacks; καθ' ἑκάστην ἡμέραν ἕωθεν would work as well once the noun exists.

### API changes (additive; recorded in STATUS)
`vp/realise_grc.h`: GrcNP::adjFirst, GrcNP::genFirst, GrcOblique::end, GrcSub::otherwise, GrcClause::an,
GrcClause::verbFirst. `vp/transfer_grc.h`: GreekTransfer::clause(..., bool subordinate = false),
GreekTransfer::setWeekdayOrdinal(bool) (const, a mutable switch used by the engine for the alternative).
`vp/grc2x.h`: Translator::roundTripOverlap. No change in frame/, transfer/ or engine.cpp.
