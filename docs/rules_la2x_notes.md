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
