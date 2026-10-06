# Rule engine, source side and assembly (C2) — notes

Scope: `engine/rules/{include/vp/frame.h, transfer.h, cue.h, engine_config.h}`, `src/frame/`, `src/transfer/`,
`src/cue/`, `src/engine/`, tests in `engine/tests/test_rules_en.cpp`, fixtures in `tests/fixtures/rules_en/`.
Pipeline per DESIGN §10: cues -> sentences -> tokens (contractions) -> tagger/parser -> lexicon lemmas -> phrasebook
pre-pass -> units (phrase pieces + clause frames) -> `Transfer` -> C1 `LatinRealiser` -> display (macron overrides,
macrons option) -> pieces back on the cues -> C1 checker + A5/A7/A8/A9 -> confidence, reasons, alternatives.

## What the frame builder handles
- Cue mapping: a cue without terminal punctuation continues into the next speech cue; speaker dashes, `♪` songs,
  `[..]`/`(..)` nonverbal groups (also inside a speech cue), abbreviations (Mr. Dr. St. Sra. ...), "..." continuations.
  Batches: `prevSource` / `nextSource` are virtual cues, so a sentence that crosses a CLI batch boundary is translated
  once and gives the same pieces in both batches (tested); the previous cue also seeds the discourse memory.
- Tokens: the contraction table before tagging (offsets of the original word kept), generic `n't 're 've 'll 'd 'm 's`,
  possessive `'s`; Spanish `del`/`al` and enclitics on verbs (`Dámelo` -> `Dá me lo`, deepest base that is a verb).
- Lemmas: english.vpl / spanish.vpl analyses filtered by UPOS, preferring analyses whose person/number/mood do not
  contradict the tagger (`sé` = saber, not the imperative of ser); inflected forms prefer a lemma other than the form;
  fallback `vp::nlp::ruleLemma`.
- Repairs of frequent tagger/parser slips: a sentence without a verb gets one retagged and re-parsed ("Light the
  candle.", "My mother teaches children.", "He does not like fish."); a noun root carrying the real clause as `acl`
  is re-rooted (time noun -> time oblique, else subject); an `nsubj` with its own preposition is an oblique
  ("Behind the door was a garden."); "Am I mad?" (adjective tagged VERB); "six o'clock" -> hour + ordinal.
- Phrasebook: longest match over lower forms and lemmas; slots {NP} {NAME} {VP} {ADJ} {NUM}, optional `(..)`,
  alternatives `a|b`, contractions inside patterns. A match is used when it stands alone between punctuation /
  conjunctions, or as a prefix for adverbial / answer / polite rows ("Of course we can talk." -> Certē + clause);
  leading "then/so/and" ride on the phrase as a connector ("Then go away." -> Abī igitur.).
- Frames (§10.1 item 5): subject/object/iobj/obliques with canonical prepositions, copula + predicate noun/adjective(s),
  existential "there", aux chain (will/shall/would/can/could/must/may/should/do/have/be), progressive, perfect,
  passive, catenatives (want to, have to, going to, let's, used to, know how to, try to), particles (go away, cut off),
  advmod, vocatives, discourse/intj, conj (clauses and NPs), mark + advcl/ccomp/xcomp with relations by marker,
  relative clauses, yes/no and wh questions (wh word, NP wh "which way", "how many"), imperatives (Mood=Imp or a bare
  verb first without subject; "Everyone bow!" -> plural with vocative), exclamations (what a .., how ..), fragments,
  negation (not, never, no, nobody, nothing, any under negation), pronouns with person/number/gender/reflexive.
- Spanish: same frames; pro-drop subject from the verb (tagger feature, else the lexicon), `usted(es)` 2nd person,
  `¿ ¡`, clitics as arguments (`me te le nos ...` without preposition = indirect object), `por favor`, subjunctive and
  conditional from the tagger's mood, markers porque/cuando/si/para/aunque/que, `ir a` future, tener miedo/frío/hambre.

## Transfer
- REVX candidates filtered by POS (adjective slots accept participles at -0.05), scored per §10.2: base/255, tier term
  by fidelity, +0.1 for T1 at fidelity >= 2 (style target "T1 whenever the sense is exact"; measured: without it the
  regression drops 75 -> 73), -0.2 defective lemmas (for "fārī" vs loquor), sense-keyword overlap with the clause
  (<= 0.2), verb tags (transitive/intransitive/with-dat), Spanish noun gender (+0.15, hijo -> fīlius), corrections
  (+1.0); periphrasis_la.tsv single-word rows at fidelity >= max(2, tier). Ties: lemma id.
- Closed classes (src/transfer/tables.cpp): pronouns, determiners (this/that -> hic/ille, no/any -> nūllus/ūllus,
  every/all -> omnis), possessives (meus/tuus/noster/vester; 3rd person suus when the subject agrees, else eius),
  prepositions by context (to: dative for persons with non-motion verbs; with: cum for persons, bare ablative for
  things; time nouns -> bare ablative / accusative of duration; languages -> Latīnē ...), verb+preposition senses
  (live in -> habitō, look at -> spectō, wait for -> exspectō + object, depend on -> pendeō ex), phrasal verbs
  (go away -> abeō, cut off -> abscīdō ...), state adjectives as verbs for persons (afraid -> timeō, cold -> frīgeō,
  wrong -> errō), weekdays (diēs Mārtis), numerals and ordinals, interjections, connectors (then -> igitur, or
  deinde after "first").
- Tense: past events perfect, states/progressive/habitual imperfect (Latin-side state list), pluperfect, future,
  "shall I" deliberative subjunctive, "would" imperfect subjunctive (present indicative at fidelity 3), present
  passive without agent -> perfect passive ("is broken" -> frāctum est), modals possum/dēbeō/volō/nōlō, let's ->
  1st plural subjunctive, "before" after a hortative/imperative -> antequam + subjunctive.
- Discourse memory: last noun gender/number ("This one does." -> Haec currit; "red ones" -> rubrās), last verb
  (ellipsis "does"), "first" -> deinde, addressee plural from the previous cue (imp.number; marked Check).
- Unknown words: never guessed; the source word stays in brackets, the token is unknown, confidence Fix.

## Engine (makeEngine)
- `EngineConfig {dataDir, curatedDir, nlpDir, advisors, cpsLimit, maxLine, maxLines}`; `makeEngine()` reads
  VP_DATA_WORK / VP_CURATED_DIR / VP_NLP_DIR (defaults data/work, data/curated, <dataDir>/nlp).
- Checks: C1 A1-A4/A6 (tier ceiling 3/2/1 by fidelity), A5 markup, A7 coverage (content tokens neither translated nor
  dropped with a reason), A8 cps (> 17) and overflow, A9 stub "not implemented". Confidence §10.4.
- Reasons per token: realiser form/name reasons, "sense" (source -> lemma, score), "candidate" (JSON list of
  candidates with scores and why), "phrasebook", "correction", "evidence" (model/online). Alternatives (<= 3):
  second-best lemma of the most ambiguous word (margin < 0.3), feminine speaker when speakerGender is 'u'.
- Whole-cue corrections (key = en_key/es_key of the cue source, as the CLI stores them) replace the translation.
- Coordinator decisions applied: `Options.speakerGender`, imperative number from the previous cue, macron overrides
  (data/curated/macron_overrides.tsv, loaded by the engine because C1's loader does not read it), -ne on the verb.
- Display also drops combining tie bars / double breves (U+035C-0362) that C1's displayForm keeps ("de͡inde").

## Measured (fidelity 2, speaker f, data/work of 2026-10-06)
- Regression own_dialogue.en.srt: **75 / 114** normalised matches (any gold alternative). Confidence ok 44, check
  70, fix 0. 99 cues have a T1/T2 candidate for every content word; all 99 are bracket-free. Determinism: two fresh
  engines byte-identical. 114 cues in ~0.15 s. RssAnon flat over 1,026 cues (CLI-sized batches of 20).
- Frame builder: 40 / 40 own sentences as expected (tests/fixtures/rules_en/frames_en.tsv).
- Caveat: the regression file was used for tuning (DESIGN D14); the held-out number will be lower.
- Mismatch classes (39): lexical choice within the right sense family (14: cantus/carmen, hōrārium/hōrologium,
  subrīdeō/rīdeō, littera/epistula, ignōtus/mīrus, pōnō/serō, corium/pallium, fīlia for "child"), addressee number
  not inferable from the text (3: pingis/pingitis, iuvā/adiuvāte), word order variants (3: "Valdē parva et valdē
  alba est", "Semper hīc ...", "Quī diēs hodiē est"), parser failures (6: #56-58, #61, #97, "Which way should I go"
  with way as object -> agere), idioms needing paraphrase (5: impossible -> fierī nōn potest, bow -> sē inclīnāre,
  play cards -> chartīs lūdere, pass me -> dā mihi), C1 form (domū for domus abl.), others.

## Known gaps by construction
- Relative clauses with prepositions ("the house in which") and stranded prepositions; indirect questions are
  passed as SubRel::IndirectQ but the realiser's mood/connective for them is C1's default.
- Coordinated predicate adjectives with separate adverbs keep one copula at the end.
- "you" number is guessed (singular unless "all", plural vocative or previous-cue addressee).
- Digits as numerals are not rendered (A7 reports them); ordinals only for o'clock.
- Tag policy (`cue::applyTags`) is implemented and tested, but the Engine API receives plain text: the CLI keeps
  tags at export (its README); A5 "tag position approximated" needs the spans, i.e. a CueInput field.
- Spanish: no phrasebook_es_la.tsv yet; REVX gaps (es:rosa has only rosārius); the Spanish parser LAS is 80.

## Questions for the main agent
1. Should the CLI adopt `makeEngine(EngineConfig)` with nlpDir = `<lexicon dir>/nlp` (see STATUS API changes)?
   With the default `makeEngine()` the server test's data folder has no models and translate returns
   `lexicon_missing` (hint names the files).
2. test_server.py assumes the stub (echo text, reason "stub engine"); with VP_HAVE_RULES=ON and the models it fails
   only those 3 checks. Who adapts it (CLI owner)? Until then VP_HAVE_RULES stays OFF by default.
3. A5 for tags: add `CueInput.spans` (or a tags flag) so the engine can report "tag position approximated"?
4. The phrasal / verb+preposition / state-adjective tables live in src/transfer/tables.cpp; move them to
   data/curated/ (phrasal_en_la.tsv) for the teacher to edit?
5. Gold-style questions: "Valdē parva est et valdē alba" (repeat the copula per adjective?) and "Quī diēs est hodiē"
   (wh questions: verb right after the wh phrase?) are realiser order rules (C1) if you want them.
