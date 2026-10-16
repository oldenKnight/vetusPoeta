# Latin -> English / Spanish (C11, `engine/rules/src/la2x`) — notes

Files: `engine/rules/include/vp/la2x.h` (Analyser, Translator, views), `src/la2x/` (`analyse.cpp` readings and
constraint search, `readable.cpp` Latin roles -> `frame::SemFrame`, `realise_en.cpp`, `realise_es.cpp`, `morph_en.cpp`,
`morph_es.cpp`, `tables.cpp`, `translator.cpp` lexical choice / names / interlinear / cues / A9), tests
`engine/tests/test_rules_la2x.cpp`, fixtures `tests/fixtures/la2x/{sentences,tokens}.tsv`, tables
`data/curated/readable_en.tsv`, `readable_es.tsv`, rows appended to `gloss_es_la.tsv`.

## Pipeline
1. Tokens: words (macron letters, combining marks), numbers, punctuation; sentences split at . ! ? (and ; before a
   capital). `morph::analyseLatin` per word; when the whole word is known only as a rare lemma an enclitic split is
   added as a competing reading (vidēsne = vidēs + -ne, not "viden"). Text with any macron is read strictly (a reading
   must carry the written length marks; anceps forms pass). Weak readings (symbol lemmas, letter names, caseless
   citation entries, alternative spellings) are dropped when others exist. Names: lexicon proper names, every declined
   form of names_la.tsv (not the "translate" title rows), the project glossary, capitalised unknowns (Check).
2. Pre-filters (deterministic, documented in code): cum = conjunction at a clause start in a two-verb sentence, else
   preposition; quod = conjunction unless a neuter noun precedes; quam = exclamative first in "!", "than" after a
   comparative; question-initial quō/ubi/unde/cūr... = adverbs; domī/rūrī = locatives.
3. Priors per reading: tier (curated tiers win), frequency rank, Whitaker frequency, flags (late, poetic, alternative),
   canonical readings (vocative, locative, subjunctive, imperative, gerund, future perfect, participles, passive of
   non-deponents and the perfect spelled like the present get penalties), capitals (name mid-sentence; a core common
   noun first in the sentence is not a name).
4. Beam search (96) left to right with pair terms: preposition -> case; NP agreement (case, number, gender, distance
   <= 3, no verb/preposition between); subject-verb person/number (coordinated subjects, copula predicates); valency
   (valency_la.tsv, else sense tags; giving/saying verbs take a dative; a bare ablative of a person is unlikely);
   genitive attribute; nōlī + infinitive; ō + vocative; relative pronoun vs antecedent. Clause terms when a clause
   ends: one finite verb, verb-final bonus, one nominative subject, modifiers that agree with something, ut/nē +
   subjunctive, main-clause subjunctive only hortative/deliberative. Clauses come from a segmentation (subordinators,
   relatives after a noun, et/sed between two verbs, ; :) of the current best reading; repeated until stable (<= 3).
5. Confidence per token = softmax share of the chosen reading among the distinct readings within 1.5 of the best
   analysis (1.0 when one survives); alternatives listed in the interlinear word.
6. Frame (SemFrame, lemma strings = Latin keys): NPs (PPs, agreement, one determiner per head, genitives, -que/et
   coordination, quot/numerals), predicate (finite verb, periphrastic passive, modal / catenative + infinitive, acc +
   inf after saying/thinking incl. esse + predicate, jussive iubeō + acc + inf, nōlī + inf, ablative absolute -> Time
   sub marked Check), roles by case, copula predicate, existential (copula first or nūllus/multī), dative of
   possession, floating omnēs, relative clauses attached to the antecedent, subordinators by marker.
7. Realisation: see the header comments of realise_en.cpp / realise_es.cpp. Articles: subject, indirect object and
   prepositional object definite; object, predicate first mention indefinite; plural objects and mass nouns bare;
   previous mention, genitive, "unique" nouns definite. Imperfect = was ...ing (simple past for states), perfect =
   simple past, pluperfect = had + pp; purpose ut + subj: "to" + inf with the same subject, else "so that" + present.

## Measured (real latin.vpl, 2026-10-06)
* Own set `sentences.tsv`: 125 sentences, English 125/125, Spanish 125/125 (normalised exact match). 87 were written
  first and tuned against; a second batch of 30 written afterwards came out 20/30 right in both languages on the
  first run (misses: substantive adjective after a preposition, acc + inf PP, impersonal pluit, estar sentado,
  predicate nominatives with pronoun subjects, quaesō, Spanish glosses) and was then fixed and added, plus 8
  sentences from the back-translation work.
* Tokens `tokens.tsv`: 335/335 lemma + features (hand-reviewed).
* Back-translation of the 114 gold lines (`<build>/la2x_backtranslation.txt`): mean content-lemma overlap of our
  English with the source 0.78; mean A9 gloss overlap 0.83. Worst lines are idioms (Libenter -> "Gladly" for "You're
  welcome", aufūgit, minimē), synonyms (fear/afraid, speak/talk, laugh/smile) and two hard sentences (quā viā eās with
  interest; id pendet ex eō quō).
* EN->LA with the A9 hook: own_dialogue 0 extra Check (confidence still ok 69 / check 45 / fix 0).
* 1,000 sentences ~0.25 s; RssAnon flat (1,900 kB -> 1,900 kB); two translators byte-identical.

## es-MX choices
tú / ustedes (2nd plural conjugated as 3rd plural, possessive of vester = su), never vosotros or usted; clitics before
the finite verb, attached to affirmative imperatives and infinitives with the written accent (dámelo, escúchame,
inclínense); personal "a" for persons and names; ser / estar: estar for "estar" adjectives in readable_es.tsv,
locations and ubi questions; "hay / había" for existentials; "no ... nada"; wh questions invert (¿Dónde está Marco?),
yes/no questions keep SVO (¿Julia está en el jardín?); nōnne -> "¿La niña no ...?", num -> "¿Acaso ...?"; ille =
aquel, iste/is = ese; Spanish noun gender from the Spanish word (bosque m, flor f), else the Latin gender; recámara,
"en la mañana".

## Known gaps
* Idioms beyond the small phrase table; senses are the first gloss (capiō = take, facere = make except quid facis).
* Gerund, gerundive, supine, future participle, indirect questions, conditional sequences (sī + subjunctive is
  rendered as indicative), cum + subjunctive causal/concessive senses, ablative of comparison, oblique cases of
  quī after prepositions beyond in/cum, double accusatives (rendered as two objects), ellipsis ("Haec currit" for
  "This one does").
* Adjectives that are also perfect participles (frāctum est) are read as adjectives when the lexicon prefers them.
* Lexicon data: some lemmas have wrong flags/glosses (ager plural-only, serō homographs); readable tables override
  the core vocabulary only.
* Tokens of la-en / la-es cues describe the Latin SOURCE words (offsets in the cue source; flag "source-tokens").

## Engine wiring (engine/rules/src/engine/engine.cpp)
One block "C11 la2x ... BEGIN/END" at the end of RulesEngine plus five one-line hooks marked "C11 la2x hook": the
include; translate() dispatches la-en / la-es to `la2x::Translator::cues`; inspect() overlays the curated glosses for
Latin; translateImpl() hands the cue's non-table source lemmas (transfer::Choice::source) to la2xSourceLemmas()
before runChecks(); runChecks() asks la2xA9() (overlap >= 0.5 ok, else Check; check() of an edited cue: "not
implemented for edited cues"). The translator is rebuilt when the Latin lexicon pointer changes.

## Questions for the main agent
1. Articles: subjects are always definite ("The girl loves a rose"); first-mention objects indefinite. Keep, or
   definite objects when the noun is "known" in the scene (Alice dialogue)?
2. Imperfect: "was working" (progressive) for events vs simple past for states; "used to" never chosen. OK?
3. Superlative = "very X" / "muy X" when there is no partitive genitive. OK?
4. CLI/UI (C8): la-en tokens are source tokens; `reasons` kind "analysis" carries {lemmaId, head, gloss, glossLang,
   pivot, form, role, confidence, alternatives, why}; `alternatives[0]` is a word-by-word gloss line. Show them?
5. Should readable_*.tsv grow into a teacher-editable core-vocabulary gloss list (now ~230 content words)?

## C11b: analyser fixes found by Orbergise (2026-10-06)
Files: `src/la2x/{analyse,readable,translator,realise_en,realise_es}.cpp`, `internal.h`, `vp/la2x.h` (one additive field),
`src/check/check.cpp` (A6 hunk only), tests `test_rules_la2x.cpp`, fixtures `sentences.tsv` / `tokens.tsv`, rows in
`readable_en.tsv` / `readable_es.tsv` (la2x's own tables).
1. **Lemma preference** (priors in `analyse.cpp`): homographs of one kind (verb / verb incl. participles, noun / noun incl.
   adjectives) prefer the core lemma (tier 1/2 over tier 3: -0.5; tier 1 over tier 2: -0.2; participles count at their
   verb's tier); across kinds the clause decides (lacrimās is a verb where the clause needs one). A form known only from
   a form page while another lemma's own table has it loses 0.8 (volat: volō "fly", edit: edō "eat"). est / sunt / erat
   ... are always sum. A pluperfect not spelled -erā- (pārat "from paveō") loses 1.0. Present vs perfect spelled alike
   (legit, venit, invenit, fugit; text without macrons): the present unless the sentence is set in the past (another
   past-only finite verb, heri, ōlim, nūper, prīdiē), then the perfect. domum / rūs with a verb of motion is an adverb of
   place, not a penalised object (this is what made "Mārcus domum vēnit" read vēneō). A lex row of readable_*.tsv keyed
   by a Latin key two lemmas share applies only to the lemma whose English senses name the row's word; new kind `lexh`
   gives the other lemma's word (volō fly / volar, occidō fall / caer); malformed headwords ("((caelum") do not count.
2. **Roles**: two nominatives with a verb that is not intransitive-only (valency_la.tsv, else the lexicon's sense tags)
   and no object cost 1.5 per extra nominative (was 0.6): "Puer dōnum habet", "Dōnum puer habet" read dōnum as the
   object. Coordinated nominals agree in case (+1.2: gladium et scūtum). An adjective next to sum agreeing in number is
   the predicate (laetī sunt), not a noun subject.
3. **Periphrases**: a perfect participle (participle lemma in -us or a verb's perfect participle cell, no adjective
   reading of the token) within 3 words of a form of sum in its clause, agreeing in number, scores +1.8 (adjacent) /
   +1.2, so ingressus / amātus are no longer nouns. The frame builder reads the pair as one verb: the participle's verb
   (`verbOfParticipleLemma`: the participle headword as a "verb ... participle" analysis of its verb), deponent ->
   active (secūta est = followed, locūtī sunt = spoke, ēgressae erant = had left), else passive (amātus erat = had been
   loved). Interlinear: both words keep their own entry (role verb) and carry `Word::note` ("amātus erat = had been
   loved (one verb: pluperfect passive of amō)", Spanish "había sido amado (un solo verbo: pluscuamperfecto pasivo de
   amō)"), also pushed to the token's `why` (UI "Why this reading?") and to the reasons data (`note`); the word-by-word
   line gives the verb form once. readable_en: verbs tagged direct-in / direct-ex take in + acc / ex + abl as a plain
   object (entered the temple, left the house), from-ex renders ex as "from" (set out from the city).
4. **A6** (check.cpp): a participle lemma counts at its verb's tier (lowest tier among the verbs listing it as their
   participle); a hint naming the verb matches the participle reading. Test: "Epistula lēcta est." and "Puella Mārcum
   secūta est." pass A6 at ceiling 1 (the second failed before), a tier-3 word still fails.
5. Also fixed from the blind batch: relative pronoun after a preposition (in quā, cum quā) opened two nested clauses;
   a restrictive relative clause or a genitive attribute makes the noun definite (also as a predicate); quantifiers /
   demonstratives standing alone as the head (ab omnibus: by everyone / por todos; quam ille after a thing: that one /
   aquel); an adverb after comparative quam (than yesterday / que ayer); comparative lemma of celer; Spanish mejor /
   peor; fons, clārus (bright / claro).

Cases added: tokens.tsv 45 rows (33 homograph readings incl. vēnit/venit/legit/est/volat/vīs/canis/portās/eō/edit/
occidit/occīdit/caelō, 8 neuter objects, 4 periphrasis participles; optional 5th column = English interlinear gloss,
checked), sentences.tsv 17 role sentences + 17 periphrases (15+ required; the new test checks both words carry the note
and the role verb) + blind batch 2 (40) + 2 relative-with-preposition sentences.

## Blind batch 2 (C11b)
40 new beginner-reader sentences (deponents, passives, relative clauses, indirect statement, questions, imperatives,
ablative absolute, comparatives) written with their expected English and Spanish AFTER items 1-4 were done, run once.
**First run: English 31/40, Spanish 25/40** (strict normalised match against the expectations as first written).
Misses: fōns glossed "water issuing from the ground"; celerior in Spanish ("más comparativo de celer"); quam ille ->
"than he"; ab omnibus -> "the everies"; antecedent of a relative clause and a predicate with a genitive got "a/un";
laetī read as the noun laetus; "in quā" relative broke the sentence; meliōrem -> "más bueno"; clārius quam herī lost
"than" and glossed "famous"; plus 11 Spanish/English wording differences where the engine's reading is equally correct
(está de pie / parado, mi hijo / hijo mío, surge / sale, mucho tiempo / por mucho tiempo, rápido / rápidamente, se
alejaron / se fueron, Hear me / Listen to me, Óyeme / Escúchame, gave to me / gave me, by everyone / by all, es / está
más claro). Counting those as right, the first run was 32/40 in both languages. After the fixes of item 5 and the 11
expectation changes (listed in the fixture header): 40/40 and 40/40; the batch is in sentences.tsv (now 201 rows, all
pass in both languages; tokens 380/380).

## Left as it is (C11b)
* "Hic est liber meus." -> "This book is my." (demonstrative subject + predicate NP); "Iam nox est." -> "The night is
  already."; "Populus Rōmānus" reads Rōmānus as a name; hostis glossed "an enemy of the state" (no readable row).
* Article policy unchanged: first-mention objects stay indefinite ("The girl followed a mother").
* Spanish "sēdit" in a coordinated clause gives "estuvo sentado" without agreement.

## Real prose and liturgy (C36)
Material: the owner's Latin of a textbook chapter (14 units, la->en) and a medieval hymn (22 lines, la->es) in
data/acceptance/ (gitignored, never quoted; units by number only); own material written before any rule:
`tests/regression/own_prose.la.txt` (60 textbook-prose sentences) and `own_prayer.la.txt` (30 devotional lines of our
own), gold in `tests/regression/expected/own_{prose,prayer}.{en,es}.gold.txt`; a blind set of 20 further lines in
data/work/eval/c36-blind/ (gitignored). Tests: `engine/tests/test_rules_la2x3.cpp` (report `<build>/la2x3_report.txt`).

### Step 1: calibration (coverage.cpp)
After realisation `detail::Coverage` compares the sentence with the Latin analysis and adds flags that make the cue
Check (check id `meaning`, `SentenceOut::coverage` holds one note per finding): `meaning-dropped` (a content lemma whose
target word or an inflected form is missing; Spanish verbs and adjectives through morph_es, clitics allowed),
`meaning-added` (an output content word no Latin word accounts for; the closed-class rows of the readable tables and the
forms of be / ser / estar / haber / ir are not content), `existential-copula` (there is / hay for a subject with a
descriptive adjective and no place), `dangling-conjunction`, `gender-mismatch` (Spanish adjective / participle against
the Spanish noun it agrees with, else the Latin gender), `vocative-lost` (a capitalised noun with a vocative reading read
as a bare ablative / dative in a sentence that speaks to someone), `pronoun-guess` (a restored "it" with no neuter
source), `article-guess` ("without the" for a first mention). On the first-run outputs (engine unchanged): la->en OK
4 -> 1 (the remaining OK unit is right), la->es OK 1 -> 0: 3 + 1 units went from OK to Check, zero wrong among OK.

### Step 2: rules
* Copula with a postposed subject: an adjective before a form of sum never attaches to the noun after it (it is the
  predicate: "the noun is adjective"); "ADJ est NOUN et ADJ2" shares the predicate; a sine phrase makes no existential.
* Postpositive autem / enim / igitur / vērō (vērō as a connector row), quoque; "et" right after a clause-initial verb
  before a noun phrase = "also" (placed after an affirmative Spanish imperative: "da también luz").
* nōn X sed Y in one clause (discourse `nonsed:X:Y`): English copula "is not X but Y", other verbs "lives not in A but
  in B"; Spanish "no ... sino"; between two verbs "does not sleep but plays" / "no duerme sino que juega"; nōn sōlum ...
  sed etiam = not only ... but also / no solo ... sino que también; "X est, sed nōn Y" = "X, but not Y".
* Adjective / noun homographs in predicate position (līber / liber / līberī): in a clause with sum (finite or esse) and
  no preposition the noun reading loses when another nominative noun is a person, or (statements only, no accusative,
  no other nominative modifier, thing-noun reading) when nothing else could be the predicate. "Ubi est liber meus?" and
  "amīcī sumus" keep the nouns. Modal + esse: nominative adjectives are predicates ("he wants to be free").
* Implicit subject with an object-first clause: while the discourse is about a person (a person subject in the last two
  sentences; `Analyser::setPersonContext`) a transitive active verb (3rd person, not an intransitive verb of motion)
  with no accusative and only neuter non-person nominatives reads them as the object; the restored pronoun is the last
  person's ("he"), not "it" for the last thing, unless a thing was the subject earlier in the same sentence.
* Names: readable `name` rows win over lexicon glosses (Davus, Iuppiter -> Jupiter / Júpiter, Iēsus, Chrīstus); two
  names side by side are one ("Jesus Christ", Spanish "Jesucristo"); a personal name is singular (Marīa never Marius);
  an initial capitalised ordinal is the praenomen (Quīntus); Marīa -> Mary only in a devotional text (else Maria).
* Avē / avēte are greetings (pre-filter on the interjection reading); salvē in a devotional text is "hail" / "salve"
  (`intj ... eccl` rows); each greeting after a comma starts its own address; a greeting line, or a verbless line with
  a vocative-only word, addresses its singular vocative readings; in a devotional text a verbless line of person nouns
  is an address (no article); adjectives addressing someone take the addressee's gender (ave, mansa).
* esse / estō / estōte are sum; deponent 2nd singular in -re is the imperative (cōnsōlāre, miserēre, tuēre); a passive
  imperative / -re passive of an active verb and the poetic perfect in -ēre lose to the infinitive (et caelum vidēre).
* nē + 2nd person subjunctive with no main verb is a prohibition (do not give / no des), also when it opens the line.
* Vocatives: a capitalised common noun inside a sentence that speaks to someone, not after a noun it could be the
  genitive of (Rēgīna caelī); a genitive stays inside the address span; plural vocatives lose in an address line.
* The addressee's gender: a 1st / 2nd person pronoun takes the gender of its periphrasis participle (tū ... ēlēcta
  fuistī: elegida); "tū sōla" keeps the adjective; a common-gender noun takes its adjective's gender (sāncta parēns:
  mother / madre via the `f:` note).
* cōnferre = grant / conceder; dā + accusative + infinitive = grant (me) to ... / concédeme ...; misereor + genitive =
  have mercy on / tener piedad de; mē + person noun = "me, a sinner" / "a mí, pecador"; mē + present participle = "me,
  who mourn" / "a mí, que lloro"; plēnus + ablative = full of / lleno de; modō + adjective = in a ... way / de modo ...
* Articles: sine + first mention = "without a master" / "sin amo" (estar in Spanish), finis "without end"; domus as the
  object of a person's verb = "his / her home" / "su casa"; meals as objects (wait for dinner / esperan la cena);
  devotional words (`devotional:` note: servant / siervo, heaven, evil, lady) without article for heaven / evil.
* Spelling: medieval ch for c (charitas), michi, nichil; a rare adjective / noun spelled like a core adverb loses
  (saepe); a thing in the dative with no dative verb loses to the ablative (modō mīrō).
* Segmentation: a comma after a complete main clause before another finite clause, or before a verbless rest with a
  second-position particle (ellipsis of est: "the master is good, but the slave is bad"), starts a new main clause.

### Numbers (vpengine serve, text project, la-en / la-es, speaker m, latinity wide, fidelity 2)
* Owner material (implementer's count, the main agent re-reviews): la->en 2/14 -> about 9/14 right, OK 4 -> 4, wrong
  among OK 3 -> 0; la->es 2/22 -> about 19/22, OK 1 -> 2, wrong among OK 1 -> 0. Still wrong: la->en 1 (misspelt
  word, Fix), 6 (second connector lost), 9 and 14 (ellipsis with et / nōn multum sed satis), 12 (predicate noun
  definite); la->es 10 (unknown -trix noun), 15 (adjective read as a verb), 22 (free relative + infinitive).
* Own material first run -> final: prose EN 18 -> 56/60, ES 18 -> 54/60; prayer EN 4 -> 28/30, ES 6 -> 30/30. The one-job
  test run keeps every mention in the discourse: 54, 53, 28, 30.
* Blind 20 (written before any rule; first run after the rules, the run happened after the usage-limit interruption):
  EN 14/20, ES 10/20, wrong among OK 1 in each (blind unit 2: villa -> house, the gold says farmhouse).
* la2x own set 201/201 EN and ES, tokens unchanged; Orbergise 60/60, 20/20, 20/20, 22/22.

### Proposed gold alternatives and the review
Accepted by the main agent and added to the gold with a note: prose 15 ES "Por lo tanto llama al esclavo a menudo";
prayer 15 EN "give the light also". Vetoed, fixed in the engine instead:
* vīlla = "farmhouse" (EN; a teacher marks "house" wrong) / "casa de campo" (ES), readable note `pp-indef`: "in vīllā"
  with no earlier mention is indefinite ("in a farmhouse" / "en una casa de campo").
* in viā after a verb of protecting / guiding / leading / keeping (tueor, servō, custōdiō, prōtegō, dīrigō, dūcō, regō,
  dēfendō, cōnservō) or in a devotional text = "on the way" ("en el camino"); elsewhere "in the road" as before.

Changed expectations (tests/fixtures/la2x/sentences.tsv, C11 set, reason: vīlla is not "house"): "Puellae in vīllā
cantant" -> The girls sing in a farmhouse / Las niñas cantan en una casa de campo; "Servī ex vīllā ēgressī sunt" -> The
slaves left the farmhouse / Los esclavos salieron de la casa de campo. la2x own set stays 201/201.

After the review (vpengine serve): own prose EN 58/60, ES 57/60, prayer EN 30/30, ES 30/30, wrong among OK 0 in all
four (test_rules_la2x3 asserts it; one-job run 56, 56, 30, 30). Blind, after a reviewer veto, not re-tuned on the blind
lines: EN 14 -> 15/20, wrong among OK 1 -> 0; ES 10/20 unchanged, wrong among OK 1 (blind 2 "Pues no vive en una casa de
campo sino en la ciudad": the rendering the review asks for; the blind gold lists "Porque ... la casa de campo" and
"Pues ... la granja" but not this combination, so it counts as a strict mismatch). Owner material unchanged (OK units
la->en 3, 10, 11, 13; la->es 6, 11).

## Frame follow-ups (C39)
Item (f) of C39 (own sentences tests/regression/own_frame.la.txt, gold expected/own_frame.en.gold.txt; tests in
engine/tests/test_rules_q.cpp):
* Abstract nouns said in general take no article in English: readable_en.tsv rows labor (work), somnus (sleep),
  amīcitia, fāma, glōria, sapientia, lībertās, iūstitia, īra, timor, amor, senectūs, ōtium (note mass; all but labor
  also note abstract); uita gets note abstract. Note abstract (new, translator.cpp / realise_en.cpp): no article as a
  subject even after an earlier mention ("O Jupiter, life is difficult." after "life" was named: owner unit 8); a
  genitive, a relative clause or an adjective brings it back ("The life of the farmer"). labor stays without abstract
  (narrative "the work is long" after a mention). Spanish keeps its article (la vida, la amistad).
* "Lūcius līber est et dominus suus": an adjective after a name subject and before the copula is the predicate
  (Lucius is free), and the et before a predicate noun is "and" (no dangling "and"); dominus / domina as the
  predicate of a 3rd-singular person with suus or joined to the predicate līber is "his / her own master (mistress)",
  Spanish "dueño / dueña de sí mismo / misma" ("Līber enim est et dominus": owner unit 12; "Mārcus dominus est" stays
  "a master").
* nōn multum sed satis: when Y of nōn X sed Y is an adverb (satis), "but" / "sino" goes before it: "He has not much
  money but enough", "His daughter works not much but enough", ", nōn multum sed satis" after the object ("provides not
  much food but enough": owner unit 14).
Owner material (by unit number, never quoted): la->en right 9 -> 12/14 (8, 12, 14 now right; 6 keeps its meaning with
"work is long" for "the work is long"), OK units 3 10 11 13 unchanged, wrong among OK 0; la->es unchanged. la2x own
201/201, own_prose / own_prayer reports byte-identical. Still open: blind LA -> EN 3 (a common-noun subject before
"līber est et dominus suus": "The free farmer is his own master and."), owner unit 9 (dangling et, nōn multum).

## Core vocabulary audit (C41)
Own material first (31efb88, before any change): tests/regression/own_core.la.txt (15 Latin sentences of core words,
gold expected/own_core.{en,es}.gold.txt), own_core.en.txt (2 EN -> LA "not very but enough" lines, gold
expected/own_core.la.gold.txt); the Greek half is in rules_grc3_notes.md. Blind 10 in data/work/eval/c41-blind/ (WRITTEN
2026-10-15 01:41 UTC). Tests: engine/tests/test_rules_s.cpp (report <build>/regression_report_s.txt).

### The audit (tools/eval/core_audit.py, test tools/eval/tests/test_core_audit.py)
Every content lemma of the DCC Latin core list (data/work/la/dcc_la.tsv, gitignored; 914 of its 997 rows are nouns,
verbs, adjectives, adverbs) was run as its citation form through `vpengine serve` (text project, la-en and la-es) and the
chosen gloss (the word-by-word line) compared with the list's English sense and the Spanish list's sense (light stemming;
verdicts ok / weak / nosense / long / leak / missing; the chosen lemma compared with the list's lemma ids), plus 40 own
audit sentences (two per 50 ranks, data/work/eval/c41-audit/sentences.la.txt). Report with heads, verdicts and outputs
only (never the list's definitions): data/work/eval/c41-audit/{first,final}/la/audit.tsv.
* First run: English flagged 317 (nosense 276, long 21, missing 18, weak 2; 164 of them rated OK), Spanish 103 (43 OK).
* Every flagged lemma was reviewed by hand as a teacher would: most are fine synonyms (big / great, want / wish, earth /
  land, take / seize, so / thus), citation-form homographs the clause resolves (aequor read as aequō, nemus as nō, vīs as
  volō, malus the apple tree, labor as lābor, nātus as nāscor; in the audit sentences they come out right), or closed-class
  words with their own rows. Wrong or odd ones (a later sense, a definition, an adverb for an adjective, a participle of
  another verb, a non-word) got a readable row: **readable_en.tsv +127 rows, readable_es.tsv +127** (lēx law was "the legal
  machinery of a state", exercitus army was "exercise", perīculum danger was "trial", imperium power was "The empire",
  castra camp was "castles", virtūs courage was "manhood", cūnctus all was "collectively", recēdō withdraw was "retire to
  one's bedroom", supplicium punishment was "kneeling", nūmen divine power was "a nod of the head", classis fleet was a
  Servian census class, condō found was "put together", dīligō love / amar was "esteem" / "elegir", ...). English output
  changed for 113 lemmas, Spanish for 64. Final run: English flagged 222 (112 rated OK), Spanish 131 (61 OK): all
  reviewed as acceptable (the Spanish count rose because the new rows use other good words than the Spanish list,
  e.g. todo for cūnctus, ejercitar for exerceō).
* Kept on purpose: transeō "get through" (A9 of "I can't get through"), cōnferō "grant" (C36 prayer), simul "together",
  errō "be wrong", precor "beg" (C36 rows).

### Calibration (translator.cpp, glossDoubts; flag gloss-doubt, check "gloss")
A content word (noun, verb, adjective, participle, adverb; not sum, not a closed-class row) whose gloss comes from no
readable_en / readable_es / gloss_es_la row and either is not the lexicon's own first sense (a Spanish gloss through the
English pivot, a gloss followed through "synonym of X") or is not a dictionary word (la2x::glossIsDictionaryWord: every
word of it, after a leading to / be / article, is a form english.vpl / spanish.vpl knows; more than three words, a
leading negation, an object placeholder or another script fail) makes the cue Check; the check "gloss" (only when found)
names the words ("unreviewed gloss: sīmiam (mono, not a first sense)"). The engine passes english.vpl / spanish.vpl
(Translator::setTargetLexicons). Effect on every suite: none of the 17 regression reports, la2x3 (own prose / prayer),
grc2x, orberg or the la2x fixtures lost an OK unit; owner material: one la -> es unit went OK -> Check (dēlectō through
the pivot), given back by a reviewed row (complacer, the same text). Wrong among OK 0 everywhere.

### Realiser fixes the audit sentences needed (la2x)
* Note pl:<word> (readable rows): a plural with a sense of its own; said in the singular (castra = the camp, litterae = a
  letter, aedēs = a house) unless the word is itself a plural ending in -s (cōpiae = the troops, verb agreement kept).
* longē / procul right before ā / ab / ē / ex: "far from" / "lejos de" ("The legion stays far from the camp"; was "stays
  from the castles long", OK and wrong).
* A copula clause that says where its subject is keeps the adjective next to the noun as an attribute ("Classis Rōmāna in
  portū est" = The Roman fleet is in the harbour; was "The fleet is Roman in the harbor", OK and wrong); "Iūlia laeta est",
  "Puella in hortō laeta est" unchanged.
* Note on for mōns, collis, īnsula (on the mountain / hill / island).
* Spanish: -iar / -uar verbs that stress the i / u (envía, confía, guía, continúa, actúa; was "envia", "confia"); -gión
  and -nión nouns are feminine (la legión, la región, la opinión; was "el legión").
* EN -> LA (frame_builder.cpp): the C39 ellipsis doubt also covers "not very / too / so / quite / really / that" between
  commas ("The house is big, not very but enough." was "Domus valdē magna est sed satis.", the opposite, rated OK; now
  Check; the Latin itself is still wrong, see Still open).

### Numbers (vpengine serve, text project, fidelity 2, speaker m, LLM off)
Own material first run -> final: LA -> EN 8 -> 13/15, LA -> ES 11 -> 13/15 (each 12 as written + 1 proposal), wrong among
OK 4 + 1 -> 0; EN -> LA "not very but enough" 0 -> 0/2, wrong among OK 1 -> 0 (both Check). The misses left are Check
(castra relīquērunt "abandoned a camp", omnibus "certain everyone"). Audit sentences (40): OK 19 -> 19 (EN), 19 -> 18
(ES); about 13 of the first-run OK units carried a wrong gloss or misreading, none after (one "into the shore" left).
Blind (first and only run 02:22 UTC, data/work/eval/c41-blind/first_run.txt): LA -> EN 3/5, LA -> ES 3/5, wrong among OK 1
each: the same unit, a prepositional phrase belonging to the subject noun read as an adverbial ("is long through the
mountains"); not tuned afterwards.

### Still open
EN -> LA "not very but enough" is Check but the Latin is still "valdē ... sed satis" (needs the nōn X sed Y tail of C40 (c)
on the Latin side); mīlle read as the noun "mile" (Check); multa as "fine" (Check); a PP after the subject noun read as a
predicate adverbial (blind 3); "in lītus" = onto the shore; Spanish personal "a" for a person noun without a row
("llama un pastor").

## PP attachment (C43)
Own material first (884aca9): tests/regression/own_pp.la.txt (17), own_pp_grc.grc.txt (13), own_pp.en.txt (2), gold in
expected/own_pp*.gold.txt; units la 12-14 / grc 10-11 have two readings and are right only as Check. Tests:
engine/tests/test_rules_t.cpp (report regression_report_t.txt).
* la2x::detail::attachPPs (readable.cpp, after buildFrames): a prepositional phrase right after an explicit common-noun
  subject of a copula clause with a predicate (adjective or noun) moves into the new SemNP::attrPP when it says where to /
  from / through / about (ad, in + acc., per, trāns, ē / ex, ā / ab, dē), or where a thing is (in + abl., sub, apud,
  cum, ...); a person with a place or a topic keeps the adverbial reading with flag pp-attach (Check, check id pp-attach).
  Not for time nouns, propter / ob / prō / contrā, or an adjective that takes the phrase itself (tūtus ā, parātus ad).
  Another verb: only dē after a thing, not after a verb of saying / thinking. Motion and position verbs unchanged.
* Realisers: the phrase after the noun (and its genitives), prep rows <case>.attr (ad / in + acc. = to / a, ē / ex =
  from / de, trāns = a través de); a noun with such a phrase is definite (the water in the river).
* Analysis: a noun / adjective homograph next to sum after subject + phrase is the predicate adjective (brevis =
  short, not "dactyl"); mīlle the numeral has no number (a thousand soldiers, not "of the mile").
* Rows: iter journey / viaje, tūtus safe / seguro (estar), angustus estrecho, stō verb+thing estar (new feature
  verb+thing: the verb said of a thing subject, Spanish only).
* EN / ES -> LA (engine.cpp): a verbless tail ", not very but enough" / ", no muy, sino bastante" is cut before the
  analysis and given back as ", nōn valdē sed satis" (valdē / multum / nimis / satis).
* Numbers (vpengine serve, text project, fidelity 2, speaker m): own LA -> EN 8 -> 17/17, LA -> ES 6 -> 16/17 (wrong among
  OK 6 + 6 -> 0), EN -> LA 1 -> 2/2. Blind (la half) 4/5 EN, 4/5 ES, wrong among OK 0. All other reports byte-identical
  except regression_report_s (templum stat = está, not "está de pie"; not very but enough right). Owner material
  identical in all nine directions.
