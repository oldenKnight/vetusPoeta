# Greek realiser notes (C9) — what is implemented, lexicon gaps, questions

Greek counterpart of C1 (`docs/rules_la_notes.md`). Code: `engine/rules/{include/vp,src}/{morph_grc,realise_grc,check_grc}`;
tests: `engine/tests/test_rules_grc.cpp` with fixtures in `tests/fixtures/rules_grc/`; gold:
`tests/regression/expected/own_dialogue.grc.gold.txt` (first 40 lines of `own_dialogue.en.txt`).

## Morphology (`vp/morph_grc.h`)
* **Cells, Attic first.** A cell with the Attic extra bit scores +8, a contracted cell +4, an alternative −6; a plain
  cell whose analyses are all flagged non-Attic −10, so it is used only when nothing else fits. Cells are cleaned:
  length marks stripped (D13), a leading article inside a cell dropped ("τῆς ἀνθρώπου"), the marker "(ν)" removed
  (movable nu flag), word-final σ written ς. Multi-word cells (periphrastic perfects) are refused.
* **Contract verbs.** The tables hold both the uncontracted (plain) and the contracted (extra bit 16) present system;
  verified on ποιέω (ποιῶ ποιεῖς ποιεῖ ποιοῦμεν ποιοῦσι, ἐποίει, ποιεῖν, ποίει), τιμάω (τιμᾷ τιμῶσι τιμᾶν),
  δηλόω (δηλοῖ δηλοῦν), φοβέω middle (φοβοῦ). The contracted cell wins.
* **Voice.** Lemmas in -μαι or flagged deponent: middle (then passive) in the present system, active cells first in
  other tenses (ἦλθον, διελθεῖν). Active requests fall back to a middle future (ἔσομαι, λήψομαι). Middle for
  reflexive senses is the caller's `voice = Middle`.
* **Augment / reduplication** only from the tables; no rule generation for verbs. οἶδα (perfect cells only) serves
  the present (perfect) and imperfect (pluperfect).
* **Overrides** (5): an Attic form is used instead of the cell only when the lexicon *attests* it by an analysis of
  this lemma and these features: εἰμί impf. 2 sg ἦσθα (cell ἦς), πίνω aor. imp. πῖθι (cell πίε), δεῖ / ἔδει (the
  "Attic" table is uncontracted δέει), βούλομαι / οἴομαι 2 sg βούλει / οἴει (cell βούλῃ).
* **Closed classes** from built-in Attic tables: article, ἐγώ σύ (with enclitic μου μοι με σου σοι σε), ἡμεῖς ὑμεῖς,
  αὐτός, οὗτος, ἐκεῖνος, ὅς, τίς, τις, οὐδείς, μηδείς, εἷς, δύο, τρεῖς, τέτταρες, πᾶς.
* **Rule paradigm** (`fromRule`, Check) only for lemmas without a table: 2nd-declension nouns (-ος / -ον) and
  1st/2nd-class adjectives (-η, -ᾱ after ε ι ρ, two-termination), persistent accent recomputed (antepenult → penult
  before a long ultima; oxytones circumflex in gen/dat; penult circumflex before a short ultima). Tested on a mini
  lexicon; analysis of such forms guesses the citation form with every accent placement.
* **Analysis** undoes sentence sandhi before the exact lookup (grave → acute, the acute a following enclitic added,
  an unaccented enclitic, movable ν, orthotone ἔστι, elided δ’ ἀλλ’ ἐπ’ ἀφ’ καθ’ …); only then the accent-insensitive
  search (= "accent differs", a Check warning, never Fix).
* **Accents and sandhi** (`sandhi`, `accentuate`): οὐ/οὐκ/οὐχ/οὔ, ἐκ/ἐξ, movable ν (before a vowel, a comma and a
  sentence end), optional elision (off by default; ἐπ’/ἐφ’ by breathing), enclitic rules (oxytone host keeps the
  acute; perispomenon unchanged; paroxytone + disyllabic enclitic keeps its accent; proparoxytone / properispomenon
  host adds an acute, except properispomena in -ξ -ψ; proclitic / enclitic host takes an acute; ἐστί orthotone at the
  start, after οὐκ μή εἰ ὡς καί ἀλλά τοῦτο and when existential), grave rule (not before punctuation or an enclitic,
  never τίς / τί). Crasis is never produced. 32 cases in `enclitic_grc.tsv`.

## Realisation (`vp/realise_grc.h`, `data/curated/order_grc.txt`)
Implemented and covered by the realisation table / gold: order.decl, order.copula (two predicates: "μικρά ἐστι καὶ
λευκή"), order.exist (V S, location first; orthotone ἔστι without a wh-word), order.inf (modal before the
infinitive), order.dei (δεῖ + acc + inf), order.exesti (ἔξεστί σοι + inf), order.yn (ἆρα optional), order.yn.ou,
order.yn.me (ἆρα μή), order.wh (adverbs, τίς in its case, interrogative NPs "πόσους ἀδελφοὺς", "διὰ τί"), order.imp,
imp.number, imp.aspect (present / aorist from the clause), order.prohib (μή + present imperative or aorist
subjunctive), order.excl (ὡς + adjective + article NP; interjections with commas), order.voc (ὦ + vocative; bare
πάντες), order.conn.second / .first (read from the file: second position after the first word, also after an
article), order.art (article + adjective + noun; two adjectives: repeated article), order.art.pred (predicate noun
without the article; `forceArticle` keeps it), order.dem (οὗτος ὁ ἀνήρ), order.quant (πάντες οἱ δοῦλοι),
order.poss (μου / σου / αὐτοῦ after the noun; ἐμός … with `possEmphatic`), order.gen, order.num, order.adj (after an
indefinite noun; οὐδείς πόσος … before it, list read from the file), order.prep (emphatic pronoun after a
preposition), order.adv (time adverbs from the file first), order.rel (ὅς agreeing, case from its clause),
order.sub.pre / .cause / .purp (ἵνα + subjunctive, ἵνα μή), order.result (ὥστε + infinitive), conditions (εἰ +
indicative, ἐάν + subjunctive), neg.ou / neg.me / neg.single, pron.drop, pron.encl, agree.neut.pl (neuter plural
subject → singular verb), nu.movable, enclitic.accent, accent.grave, elision (option), punct (";" for questions, "·"
for a colon), caps (lower case; names capitalised), names (names_grc.tsv: declensions 1/2/3, vocative override,
glossary policies, unknown names kept + flag), emoji (emoji_grc.tsv, nouns only, not names).
Partial / for C2: order.dupl (the caller repeats the clause), tense.* (helper `mapTense` for the frame builder:
past event → aorist, state / progressive → imperfect, perfect only with a present result), voice.middle for
reflexive senses (the frame must set `voice = Middle`), mood.subj deliberative ("τί ποιήσω;" by `mood =
Subjunctive`). Not implemented: participles (no case cells in the tables; beginner style avoids them), optative
(never in flexible mode), dual, crasis, articular infinitive.

## Checker (`vp/check_grc.h`)
A1 (exact reading after undoing the sandhi; accent-insensitive only → warning "accent differs"), A1b (warning: re-runs
the sandhi on the lexical forms and compares; words without an accent), A3 (article ↔ following adjectives / noun,
adjective ↔ adjacent noun, predicate with copula, subject ↔ verb per clause with the neuter-plural rule and nom/acc
neuters considered only when no nominative is elsewhere, relative ↔ antecedent), A4 (prepositions: union of the cases
of preps_en_grc.tsv; valency_grc.tsv: accusative-only, dative-only and genitive-only verbs, `mid:` frames for middle
forms), A6 (min of lexicon tier and tiers_grc.tsv). Closed-class words are read from the built-in tables (the lexicon
has spurious article readings of τοῦ / τόν from unrelated adjectives); table readings of a lemma win over form-page
readings of the same lemma (ἦγον is listed as 3 sg and 1 pl by form pages); ὦ before a vocative is not the
subjunctive of εἰμί. Results: checker table 49/49, generator 2,000 clauses with 0 A1/A3/A4 faults and 0 accent
warnings, 200/200 corruptions caught (article gender, noun case inside an article phrase, adjective gender, verb
number against an expressed subject).

## Lexicon gaps found (for LIB / B4b)
1. Greek particles carry pos 13 (participle): `tagmap.py` matches the head template "grc-part…" (particle) as a
   participle (οὐ, μή, οὖν, γε, ἆρα, δέ as particle). The engine treats pos 13 without case or mood as a particle.
2. Plain (unflagged) noun / adjective tables are often Epic (ἀνθρώποιο, ἀνθρώποισι, κᾱλοῖο) with the Attic table
   flagged; 165 common-gender lemmas carry the article inside their Attic cells ("τῆς ἀνθρώπου").
3. 729 cells write a final σ (ἦσ, εἴησ); 20 cells carry "(ν)"; εἰμί Attic impf. 2 sg is ἦς (ἦσθα attested).
4. δεῖ: the "Attic" table is uncontracted (δέει, ἔδεε). πίνω aor. imp. cell πίε (πῖθι only on a form page).
5. ἀποφεύγω has no aorist cells (the realiser reports `missing-form`); verbs in -μαι are often not flagged deponent
   (ἔρχομαι, ἐπανέρχομαι): the engine uses the -μαι ending.
6. Tables have participles only as masculine / feminine / neuter nominatives without case.
7. Tiers: most beginner words are tier 3 in the library (θύρα, κῆπος, παίζω): 211 "derived" rows were added to
   tiers_grc.tsv (Athenaze-style core of the tests) until the teacher's list exists.
8. No Attic word for "tea"; ἀντιβολέω lacks the sense "entreat" (gloss "meet by chance").

## Questions for the main agent
1. **Tea**: the gold uses the hypernym ποτόν "drink" (lines 36–37); alternative: keep "[tea]" unknown (Fix).
2. **Please**: ἀντιβολῶ (Aristophanic "I beg you") after a comma; or drop it in flexible mode?
3. **Movable ν before a comma**: applied (Smyth: end of clause); Athenaze-style texts vary. Keep?
4. **Word order of yes/no questions with an object**: SOV ("ἆρα τὴν γαλῆν μου εἶδες;"); the verb-first variant is in
   the gold as an alternative. Prefer one?
5. **Lower-case sentence starts** (Oxford / Athenaze practice) in subtitles: confirm with the owner.
6. **Names with the article** by default in narrative ("ὁ Μᾶρκος"): the frame builder sets `definite`; confirm, and
   confirm the coined forms (Ἀλίκη for Alice).
7. **Imperfect 1 sg of εἰμί**: ἦ (ἦν before a vowel by the movable-ν mechanism). Accept, or always ἦ?
8. **Imperative aspect default** for the frame builder (present vs aorist): Athenaze uses both; proposal: aorist for
   single concrete acts with an object ("δός μοι", "ἄνοιξον"), present otherwise.
9. **Identifying predicates** ("She is the Queen of Hearts"): without the article by rule; the gold lists the variant
   with the article. Should C2 set `forceArticle` for titles?

## API notes
No public header of C1 or C2 changed. New headers `morph_grc.h`, `realise_grc.h`, `check_grc.h`; the Greek curated
files are read by `grc::GreekData::load(dir)` (valency, prepositions, names, particles, phrasebook, order);
`curated::CuratedData` keeps serving tiers_grc.tsv and emoji_grc.tsv. Wish for C2/C8: a Greek path in makeEngine
(SemFrame → GrcClause, mirroring the Latin mapping; `grc::mapTense` for tenses) and the phrasebook_en_grc.tsv
pre-pass.


## Decisions by the main agent (2026-10-06), binding for C9/C12
1. Realia without an Attic word (tea): use the nearest hypernym (ποτόν) in both modes, confidence Check, reason
   "no Attic word; equivalent used"; bracketed English only when no equivalent exists at all.
2. "Please": dropped in flexible mode (Greek politeness lives in the imperative and particles); faithful keeps
   ἀντιβολῶ after a comma.
3. Movable nu: before a vowel, before a comma and at sentence end. Keep.
4. Yes/no questions: ἆρα + verb first by default ("ἆρα εἶδες τὴν γαλῆν μου;"), object first only for contrastive
   focus. Mirrors the Latin -ne decision.
5. Sentence-initial capitals: yes, capitalise sentence starts in subtitles (readability, parity with Latin);
   names capitalised; everything else lower case.
6. Names: article in narrative ("ὁ Μᾶρκος"); coined Attic forms for modern names (Ἀλίκη) declined; recorded in
   names_grc.tsv.
7. εἰμί imperfect 1st singular: ἦ, ἦν before a vowel. Keep.
8. Imperative aspect: aorist for a single concrete act, present for general or continuing commands and prohibitions
   (μή + present imperative). C12 sets it from the frame.
9. Identifying predicates: a unique definite predicate (title, "the X of Y") takes the article ("ἡ βασίλεια τῶν
   καρδιῶν ἐστιν"); a classifying predicate takes none. C12 sets forceArticle for titles.
Lexicon fixes for LIB-3 (B4b): particle head templates must map to pos particle (not participle); final sigma in
729 table cells; article-bearing cells; unflagged Epic tables; δεῖ Attic table; πίνω/ἀποφεύγω gaps; -μαι deponent flag.
