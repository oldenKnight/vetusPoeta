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


## Decisions by the main agent (2026-10-06) on C2's questions
1. The CLI switches to `makeEngine(EngineConfig)` in task C8 (CLI-2), which also adapts test_server.py.
2. `CueInput.spans` added to rules.h (text/tag spans from vp::subs); the CLI fills it in C8; the cue assembly
   reports "tag position approximated" through A5 when a tag falls inside a re-broken text.
3. Yes: phrasal verbs, verb+preposition and state adjectives move to data/curated (task C2b).
4. Yes: C1 order rules for a single copula with coordinated predicates ("Valdē parva et valdē alba est" is also
   accepted in the gold) and verb-second after a wh word with the copula ("Quī diēs est hodiē?"). (C2b.)


## Quality loop 1 (C2b, 2026-10-06)
Measured with the regression test (`engine/tests/test_rules_en.cpp`, report `<build>/regression_report.txt`), fidelity
2, speaker f, data/work of 2026-10-06, gold of commit 320c3e7. The regression file was used for tuning (DESIGN D14);
the held-out number will be lower. tests/heldout/ was not opened.

### Match-rate log
| step | match | ok / check / fix |
|---|---|---|
| start (HEAD 320c3e7: restored curated rows, new gold alternatives; C2 reported 75 on the older data) | 81 / 114 | 45 / 69 / 0 |
| 1 lexical selection: curated tier (key + pos) wins over the lexicon's, teacher glosses from the tier notes, tier 1/2 of the same sense beats tier 3, beginner words, nūllus + singular, plural-only nouns, "child" in address | 100 / 114 | 63 / 51 / 0 |
| 3-5 tables in data/curated wired (phrasal, verb + preposition, states), order rules, parser fallbacks (tree repair, vp rows inside clauses, route ablative, certē / aliter, elliptical wh, "it is" complements) | 106 / 114 | 60 / 54 / 0 |
| `{WH}` phrasebook slot ("that depends on {WH}", "it doesn't matter {WH}") | 108 / 114 | 60 / 53 / 1 |
| 7 realiser fixes (macron overrides in Macrons, domō, pronoun before a name), checker: ablative relative after an ablative antecedent | 108 / 114 | 60 / 54 / 0 |
| 6 "you" number from a "we" reply | 109 / 114 | 59 / 55 / 0 |
| attributive "impossible" as a relative clause, crēdō dat (person) / acc (thing) | 110 / 114 | 60 / 54 / 0 |
| best-tier reading of phrasebook words (A6 on "es", "mē", "tē"), particles and expletive "it" counted (A7), tier rows | **110 / 114** | **69 / 45 / 0** |

Exact matches (macrons and punctuation too): 109 / 114 (#18 has "." from the source where the gold has "!").
Why the 45 Check cues are Check: best-candidate margin < 0.15: 24; A8 reading speed at the file's timings: 13;
analysis fallback (retag / reroot / clause repair): 7; "you" number guessed: 1; speaker gender in a phrase: 1.
Confidence got stricter (margin, tier 3 while a core word of the sense existed -> "low-tier", every frame fallback ->
"frame-fallback", "you" number guessed -> "addressee-guess", A5 approximated tags), yet the OK count rose because the
fixes removed A6/A7 findings (pronoun forms read as the letter S / mē lemma, particles and expletive "it" not counted,
tier 3 words replaced by core words). "you" with no evidence keeps the documented default (singular) without Check;
only a number taken from context (previous cue, "we" reply) is a guess.

### Remaining mismatches (fidelity 2)
| # | source | gold | ours | why |
|---|---|---|---|---|
| 17 | Where did everybody go? | Quō omnēs abiērunt? | Quō omnēs iērunt? | "go" without away/off is eō; abeō needs the particle (phrasal_en_la.tsv) |
| 58 | Then it doesn't matter which way you go. | Tum nihil interest quā viā eās. | Nihil igitur interest quā viā eās. | "then" of consequence is igitur everywhere else in the gold (#23, #38, #78, #109) |
| 67 | Help us, please! | Adiuvāte nōs, quaesō! | Adiuvā nōs, quaesō! | the addressee count is not in the text: the previous cues are the gardeners' own ("we", "our") |
| 86 | We live in a small house near the river. | In casā parvā prope flūmen habitāmus. | In domō parvā prope flūmen habitāmus. | house -> domus (T1, exact); casa is "hut, cottage" |

### Fidelity 1 vs 3 (same file; 10 cues that differ)
| # | fidelity 1 (faithful, tier 3 allowed) | fidelity 3 (flexible, tier 1) |
|---|---|---|
| 18 | Quam ignōtus hortus. | Quam mīrus hortus. |
| 19 | Flōrēs fābulārī nōn possunt. | Flōrēs loquī nōn possunt. |
| 24 | Omnēs valdē inhūmānī estis. | Omnēs valdē inurbānī estis. |
| 49 | In solō iānuam invēnit. | In īmō iānuam invēnit. |
| 52 | Fēlēs subrīdēre poterat. (Check: low-tier) | Fēlēs rīdēre poterat. |
| 61 | Certē es; aliter hīc nōn essēs. | Certē es; aliter hīc nōn es. (mood.cond: present indicative) |
| 68 | Dā mihi pīgmentum rubrum. | Dā mihi colōrem rubrum. |
| 85 | Māter mea fīliās docet. | Māter mea puerōs docet. |
| 90 | Crās litteram Latīnē scrībēmus. | Crās epistulam Latīnē scrībēmus. |
| 97 | Cōgitāvī diem Lūnae esse. | Putābam diem Lūnae esse. |
Confidence: fidelity 1 ok 60 / check 52, fidelity 2 ok 79 / check 33, fidelity 3 ok 48 / check 64 (2-2.5 s cues
without the srt timings, so no A8; four multi-line cues not counted). Fidelity 2 outputs equal fidelity 3 except
#61 (mood) and a few margins. Fidelity 3 used to pick tier 1 words of weak senses ("break" -> dēsinō, "mad" ->
īrātus, "school" -> grex, "hole" -> ōs): a candidate whose reverse-index score is below 0.7 of the best now loses one
more tier step at fidelity 3, and the teacher's gloss counts +1.0 there.

### What changed (C2b)
- Lexical selection (transfer/select): the tier of `tiers_la.tsv` (key + pos; homograph digit `sero2`) wins over the
  lexicon's; the tier notes are the teacher's reverse index (candidate base 0.5, bonus 0.1 / 0.5 / 1.0 by fidelity);
  at fidelity 2/3 a tier 1/2 lemma whose sense names the source word beats a tier 3 top candidate ("tier preference";
  weak senses with a base < 0.2 or < half the top's are not promoted, so "tea" stays thēa, not speciēs); a taught
  adjective may stand for a noun (bottom -> īmum); plural-only nouns found from the cells (tenebrae); "no / not any"
  + plural -> nūllus + singular ("Nūllum carmen sciō"); "child" in address -> puella / puer by Options.speakerGender.
- data/curated: `phrasal_en_la.tsv`, `verbprep_en_la.tsv`, `states_en_la.tsv` (formats in data/curated/README.md)
  replace the hard-coded tables of src/transfer/tables.cpp; `macron_overrides.tsv` is loaded by CuratedData and applied
  in realise::Macrons (the engine's own loader is gone); tier notes are glosses; beginner rows added to tiers_la.tsv:
  thēa, placenta, ientāculum, candēla, pluit, aufugiō, frīgeō, cotīdiē, anteā, maximus, Latīnus, Graecus, quārtus ...
  decimus, quotus, pēnsum, Latīnē, Graecē; notes edited: rīdeō "laugh, smile", adiuvō "help", puer "boy, child",
  epistula "letter (written), epistle", mīrus "wonderful, strange", tenebrae "darkness, the dark", pallium "cloak,
  coat"; phrasebook: the older "it doesn't matter -> nihil est" row (it shadowed the new "nihil interest" row: the first
  row wins) and the "bow -> inclīnāte vōs" row (always plural; now phrasal `bow - inclīnō refl` with the addressee's
  number) removed; added "that/it depends (on {WH})", "it doesn't/does not matter {WH}"; valency crēdō
  `dat;acc;acc+inf`; macron override thēa; order_la.txt: order.wh.cop, order.neg.degree, order.adv amended.
- Frame builder: `{WH}` slot; register `vp` rows inside clauses ("play cards" -> chartīs lūdere, "go to school");
  tree repair of clauses buried under an oblique/adverb (-> ccomp, or conj with its own cc: #61, #97); fronted adverb
  made root (-> advmod); retag after "do not" + degree adverb and of one-word imperatives tagged INTJ ("Bow!");
  clause-final "where" as an elliptical indirect question; particle and expletive "it" counted for A7;
  `SemSentence::repairs` records every fallback.
- Engine: troubled sentences (a clause unit with a verb but no predicate) are retried split at ", or / , and / , but /
  ;" (pieces joined with ";") or with discourse words dropped; every fallback -> flag "frame-fallback", Check; "you"
  plural when the next sentence starts with "we" (flag addressee-guess, Check); tier 3 chosen while a core word of the
  same sense existed -> "low-tier", Check; CueInput.spans: tag policy, flags "tags" / "tags-approximated", A5 "tag
  position approximated" is Check (other A5 findings stay Fix); phrasebook words take their best-tier reading.
- Realiser / checker (C1 files): order.wh.cop ("Quī diēs est hodiē?"), order.neg.degree ("Nōn multum cūrō"), time
  adverbs at the front only when the source fronts them, LaNP::fixed (phrasebook words), LaSub::sep ("; aliter"),
  domus ablative domō, 3rd-person pronoun subject dropped before a name in Pronouns::dropSubject (the transfer
  workaround is gone), tie bars dropped in morph::displayForm, Macrons::apply with the override; checker A3 accepts an
  ablative relative right after an ablative antecedent ("ex eō quō īre vīs").

### The main agent's new rows (heads-up of 2026-10-06)
No row made the loaders fail or crash (all loaders warn and skip; a test appends the exact rows plus broken ones to a
copy of data/curated and loads it). What went wrong was behaviour: (1) "what a {ADJ} {NP}" now wins over the frame
builder for "What a big house!", so the frame fixture's expectation at test_rules_en.cpp:392 was outdated (now
`phrase=... units=1`), and its ADJ slot was realised with a capital ("Quam Ignōtus hortus"); (2) "sero2": latin_key
drops the digit, so the row collided with sērō adv (both key "sero"; `tier(key)` returned the first row) - the loader
now reads the digit as a homograph marker and lookups go by key + pos; (3) the second "it doesn't matter" row never
applied (first row wins); (4) "bow" was plural for every addressee; (5) the `vp` rows and "which way" (register q)
could never match inside a clause. The crash near test_rules_en.cpp:750 did not reproduce on HEAD + C2b (normal and
ASan/LSan/UBSan builds); the shared tree then held other tasks' half-written files (vp/check_grc.h missing, link
errors), which is the likely cause.

### Proposed gold alternatives (for the main agent; the gold file is unchanged)
- #17 add "Quō omnēs iērunt?" - "go" alone is īre; "where did everybody go" does not say "away".
- #58 add "Nihil igitur interest quā viā eās." - the gold renders consequential "then" as igitur in #23, #38, #78 and
  #109; second position as in "Abī igitur".
- #67 add "Adiuvā nōs, quaesō!" - nothing in the text says the helper is more than one person.
- #86 add "In domō parvā prope flūmen habitāmus." - domus is the T1 exact word for "house"; casa means hut/cottage.
