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

## Quality loop 2 (C15, 2026-10-06)
Tuning material: tests/regression/oz_sample.en.srt (100 cues burned from the public-domain held-out dialogue; gold
tests/regression/expected/oz_sample.la.gold.txt by the main agent) and tests/samples/sample.en.srt. Measured with
`engine/tests/test_rules_en.cpp` (fidelity 2, speaker f), reports `<build>/regression_report_oz.txt`,
`<build>/regression_report.txt`, `<build>/regression_report_es.txt`. tests/heldout/ was not opened. The oz sample was
used for tuning: the held-out number will be lower.

### Before / after
| file | start of C15 | end of C15 |
|---|---|---|
| own_dialogue EN (114) | 114 / 114, ok 69 / check 45 / fix 0 | 114 / 114, ok 66 / check 48 / fix 0 |
| own_dialogue ES (100) | 100 / 100, ok 78 / check 22 / fix 0 | 100 / 100, ok 79 / check 21 / fix 0 |
| oz_sample EN (100) | 6 / 100, ok 7 / check 61 / fix 32, wrong among OK 5 | **50 / 100** (exact 33), ok 21 / check 60 / fix 19, wrong among OK 0 (+3 with a proposed gold alternative) |
| sample.en.srt (12) | cue 3 "Aqua colōnicior hastārum.", cue 11 "ad portōrium" | 12 / 12 as written in the test |

The target of 70 / 100 was **not** reached. Roughly a third of the remaining mismatches need a free rendering the rules
cannot derive from the English (#5, #13, #23, #30, #34, #46, #65, #74, #80, #85, #88, #97), another third are
lexical / idiom choices of the gold (#18, #21, #35, #39, #55, #70, #76, #77, #93, #98) and the rest structural gaps
listed below.

Measurement log (oz_sample): 6 start; 7 cue boundaries; 16 names, connectors, segments, lexical rows, sense head
words; 20 negative questions, coordination, complements; 25 tense/indirect question/deponent/velim; 29 passive
say/tell, look like, rogō, have no, fronted clauses; 38 comparison, particle goals, dative after impersonal adjectives,
IO pronoun after the object, plain negative questions; 42 alius, possessive on the last conjunct, motion particles;
44 lemma fixes (sting, come), literal fallback; 46 contact relatives with stranded prepositions; 48 participles as
taught adjectives, nor-inversion, trailing comma; 50 "even if I wanted to" (etiam sī vellem), "how could I" (possem),
a bare noun coordinated with the verb as a second object.

### Calibration (deliverable 6)
Wrong among OK: 0 (cues 1, 41, 82 are correct variants: proposed gold alternatives below; the test lists them).
New Check signals (frame.h `SemSentence::doubts`, flag names in CueOutput.flags): `fragment` (a cue that starts in
lower case or ends with , ; : - is a clause cut from its sentence), `contact-relative` (relative clause without a
relative word), `noun-infinitive` ("no right to take" rendered as a relative clause), `purpose-guess` (a to-infinitive
read as purpose after a verb that is not one of motion), `light-verb` ("make a visit"), `phrase-order` (a phrasebook
tail phrase placed after its clause), `participle-phrase` ("a Scarecrow, stuffed with straw"); `could-not-parse` is
Fix; `ellipsis` (a dangling "to": "if I wanted to,", "anything you want me to") is Check. Why the oz cues are Check /
Fix: fragment 38, A6 12, margin < 0.15 9, A1 9, A7 9, A3 8, purpose-guess 8, A8 7, A9 7, contact-relative 5,
noun-infinitive 5, light-verb 3, phrase-order 3, participle-phrase 3, frame-fallback 3, A4 3, ellipsis 2.
The OK share fell (36 -> 21 during the loop) because the fragment signal is strict: 38 of the 100 cues are fragments.

### What changed (C15)
- Never nonsense (1). Root cause of "The farmer carries water." -> "Aqua colōnicior hastārum.": the tagger read
  "farmer" as the comparative of "farm" (ADJ, lemma farm) and "carries" as a plural noun; the verbless-sentence retag
  required a noun before the -s word. Now a tagged ADJ/VERB that english.vpl never reads that way takes the lexicon's
  reading (lexicon veto), the retag accepts it, and a clause made only of nominal words around one verb is rebuilt
  (verb root, subject, object). Lemma fixes: a rule lemma that is not a lemma of the tagged part of speech ("sting" ->
  "st") or an all-caps abbreviation ("ST") is never chosen; "have come" keeps come (not the homograph "cum"). When the
  parse still has no clause for the source verb (or a statement of four or more content words has no verb at all while
  a noun can be one), the sentence is given word by word in dictionary forms, unknown words in brackets, flag
  `could-not-parse`, confidence Fix, reason "could not parse".
- Sense head words (2): the head of a gloss item (last word before a preposition for nouns/adjectives, first word for
  verbs) matching the source word: +0.05 when it is the first item; the word only inside an item as a modifier: -0.3
  (nouns/adjectives; "harbour" inside portōrium's "upkeep of public harbours"). British spellings also look up the
  American key (harbour -> harbor, colour, centre, realise). An intransitive sense with an object: -0.25 (hurt ->
  noceō, not doleō). Determiner lemmas may stand for adjectives (alius).
- Token hygiene (3): `morph::cleanHead` strips editorial marks from headwords ("((caelum", "((alius", "((locus" are in
  latin.vpl); `displayForm` applies it, so lookups (findLemma) and every displayed form are clean; "[x]" unknown
  markers stay. The checker tokenises on word characters only (A1 re-analysis sees the same words).
  **For C8b (CLI, not this task's directory):** words.list prints `lex.lemma(id).head` raw; it should print
  `vp::morph::cleanHead(head)`. **For LIB:** latin.vpl carries headwords with "((" (data bug).
- Fragments and discourse (4): a cue ending with , ; : or a dash no longer continues into a next cue that starts a
  clause (a capital or a conjunction / subordinator); "I was walking" + "to the river." still joins. Segmented parse:
  a sentence-initial discourse word before a comma, a vocative between commas (my dear, comrades, your Majesty), a
  parenthetical between commas (you know, as I said, however, I suppose ...) and each clause after ; : or a dash are
  parsed apart. Lead connectors set off by a comma open the next clause (however -> tamen, besides -> praetereā,
  therefore -> itaque, for -> nam, still -> tamen); "for" + subject pronoun + verb is the conjunction nam; "Then" in a
  statement -> tum (first), in an imperative / question -> igitur (as before); "and then" -> et tum; adverb "then" ->
  tum. Phrasebook registers: `lead` (only before a comma at the start: "Why," -> Quid?, "Well," -> Bene), `paren`
  (you know -> ut scīs, as I said -> ut dīxī), `conn` (", however;" moves to the front of its clause: Tamen ...),
  `tail` (closes a clause: as soon as you can -> quam prīmum, in my day, in trouble). A unit ending in ? or ! takes no
  separator ("Quid? Nescīs?").
- Constructions (5): comparative predicate (-er, more, better) + than -> comparative + quam + the compared in the
  nominative after the verb; as X as -> tam X quam; so X that -> tam ... ut + subjunctive (result); verbs of ordering /
  teaching / allowing / wanting + object + to-inf -> object + infinitive (Eōs iussī ... aedificāre); help / ask / tell
  + object + inf -> ut + subjunctive with the object as subject; help / refuse / promise ... + to-inf -> infinitive;
  purpose clauses take the person of the controller; a state verb (anxious -> cupiō) and an impersonal adjective
  ("It is better for people to keep away") take the infinitive, "for X" as a dative after the verb; must / have to /
  can / could (past -> poteram) as before; "shall I / shall we?" (yes/no) -> deliberative subjunctive, "what shall we
  do" -> future; "should/would like to" -> velim; "like to V" -> libenter + V; "am I to" -> future; passives of
  deponent verbs are said actively ("quem Saga ōsculāta est"); "it is said that S ..." -> dīcitur + infinitive
  (personal); "we have been told that ..." -> nōbīs dictum est + acc + inf; "say that" -> acc + inf (as before);
  "some" -> quīdam; "no one / nobody" -> nēmō; "not ... anybody / anything" -> nēminem / nihil (one negation);
  "anything / anyone" after sī / nisi -> quid / quis; "not know" -> nesciō; "have no X" -> X nōn habeō; negated yes/no
  questions -> "Nōn potes dēscendere?" (nōn + verb first, no particle); "Why not" -> cūr nōn ...; relative clauses
  without a relative word, with a stranded preposition -> dē quō / quās (verbprep obj); look / seem like -> similis +
  dative; indirect questions -> subjunctive with the sequence of tenses; whether -> num + subjunctive; where + get /
  receive -> unde; "is gone" -> abiit; "was always" -> perfect; "tomorrow / this morning" -> crās / hodiē māne; time
  adverbs right after the subject (order.adv amended); possessive pronoun predicates (yours -> tuus est); a shared
  possessive on the last conjunct; an unstressed dative pronoun after a noun object (not in imperatives); a first
  connector before a fronted clause ("et sī quid vīs, ...") and the imperative after it verb last.
  Names (names_la.tsv "translate" rows): Oz (indeclinable, keep), Kansas -> Kansia (country: in + acc), Dorothy ->
  Dorothēa, Scarecrow -> Terriculum, Tin Woodman -> Lignātor Stanneus, Cowardly Lion -> Leō Timidus, Wicked Witch ->
  Saga Mala, Emerald City / City of Emeralds -> Urbs Smaragdōrum (place: ad + acc), Great Wizard -> Magnus Magus,
  Witch of the North -> Saga Septentriōnis, Golden Cap -> Pilleus Aureus, Power of Evil -> Potestās Malī, King of
  Beasts -> Rēx Bēstiārum. Several Latin words are declined as noun + adjective / genitive (LaAdj::before,
  LaNP::nameWords); a capitalised English common noun inside the sentence is a title, translated and capitalised
  ("the kind Stork" -> Cicōnia, "this City" -> Urbem, "my Palace" -> Rēgiam).
- Rows added: tiers_la.tsv 46 rows (portus, palea, noceō, socius, amita, avunculus, cupiō, cōnstituō, potestās,
  follis, ōsculor, fateor, saga, magus, rēgia, sēricum, cūriōsitās, tintinnābulum, omnīnō, cicōnia, pungō, arceō,
  pergō, dēscendō, arcessō, caedō, aedificō, lignātor, terriculum, stannum, stanneus, fax, praetereā, libet,
  perīculum, quīdam, quisquam, maior, apis, māne adv, vesperī, posthāc, malum2 "evil", alius det) and notes on 11
  rows (accipiō, ferō, alius, vestis, homō, dīcō, nārrō, faciō, putō, plēnus, fessus, timidus, pulsō); phrasebook 21
  rows (my dear corrected); phrasal 5; verbprep 6; states 3; preps 1 (amongst); names 14; order_la.txt: order.adv
  (time adverbs after the subject, imperative), order.adj (alius, ūllus before the noun).

### Remaining oz mismatches (fidelity 2)
| # | source | gold (alternatives with /) | ours | confidence, findings |
|---|---|---|---|---|
| 1 | Therefore we still have witches and wizards amongst us. | Itaque adhuc sagae et magī inter nōs sunt. | Itaque adhūc sāgās et magōs inter nōs habēmus. | ok|
| 2 | He is more powerful than all the rest of us together. | Potentior est quam nōs omnēs ūnā. / Potentior est omnibus nōbīs ūnā. | Ūnā fortior est quam omnēs requiētēs nostrī. | fix A3 |
| 4 | Can you help me find my way? | Potesne mē adiuvāre ut viam inveniam? | Potesne mē adiuvāre ut viam meam inveniam? | check A8 emoji cps |
| 5 | there is a great desert, and none could live to cross it. | magna dēserta est, et nēmō ea trānsīre vīvus potest. / ingēns dēserta est, nec quisquam vīvus trānsīre potest. | Est dignitās magna et nēmō vīvere poterat ut eam trānseat. | check purpose-guess fragment |
| 12 | and no one will dare injure a person who has been kissed by the Witch of the North. | et nēmō audēbit nocēre eī quem Saga Septentriōnis ōsculāta est. | Et nēmō hominī quem Sāga Septentriōnis ōsculāta est nocēre audēbit. | check fragment |
| 13 | They would be just the thing to take a long walk in, for they could not wear out. | Ad longum iter aptissimī essent, quia numquam dēterī possent. / Ad longum iter optimī essent, quia numquam terī possent. | Rēs quae viam longam intrō sūmit et gallus nōn essent. | fix A3 A6 A7 noun-infinitive light-verb participle-phrase emoji |
| 14 | Besides, you have white in your frock, and only witches and sorceresses wear white. | Praetereā album in veste habēs, et sōlae sagae albō vestiuntur. / Praetereā album in veste tuā est, et sōlae sagae alba gerunt. | Praetereā habēs ut album in veste tuā et modo et venēficae ut portet ut album [witch]. | fix A1 A6 A8 purpose-guess emoji overflow |
| 18 | I feel like a new man. | Novus homō mihi videor. / Quasi novus homō sum. | Vir novus mihi videor. | fix A3 emoji |
| 21 | it’s a lighted match. | fax accēnsa est. | Certāmen est [lighted]. | fix A1 A9 fragment |
| 23 | It is such an uncomfortable feeling to know one is a fool. | Molestum est scīre sē stultum esse. / Valdē molestum est scīre sē stultum esse. | Sēnsus incommodus quī ūnum hominem scit est et mora est. | check noun-infinitive |
| 26 | They are rusted so badly that I cannot move them at all; | Tam rōbīgine corrupta sunt ut ea movēre nōn possim; / Tam rōbīginōsa sunt ut ea omnīnō movēre nōn possim; | Rōbīginātī sunt ut eōs apud omnīs movēre nōn possim [so badly]; | fix A1 A6 A7 fragment |
| 30 | When they scratched against the tin it made a cold shiver run down my back. | Cum stannum rādēbant, frīgus per tergum meum currēbat. / Cum stannum rādēbant, frīgore horrēbam. | Quandō contrā stannum quod tremōrem frīgidum fēcit tergō meō cucurrisse scalpsērunt [down]. | fix A1 A6 A8 contact-relative light-verb cps overflow |
| 33 | but that doesn’t make me any braver, | sed hoc mē fortiōrem nōn facit, | Sed id [braver] mē facit, | fix A1 unknown fragment |
| 34 | for you can carry us all over on your back, one at a time. | nam nōs omnēs trāns portāre potes in tergō, singulōs. / nam nōs omnēs singulōs in tergō trāns portāre potes. | Nam omnīs in tergō tuō super portāre potes et ūnus homo tempore. | fix A3 fragment |
| 35 | or the Tin Woodman badly dented on the rocks below. | aut Lignātor Stanneus in saxīs īnfrā graviter contūsus. / aut Lignātor Stanneus in saxīs īnfrā graviter laesus. | Aut Lignātor Stanneus in saxīs male īnfrā [maccare]. | fix A3 A6 missing-form fragment emoji |
| 37 | but the kind Stork saved me, | sed benigna Cicōnia mē servāvit, | Sed Cicōnia benigna mē servāvit, | check A8 fragment cps |
| 38 | I always like to help anyone in trouble. | Semper eōs quī in perīculō sunt adiuvāre libet. / Semper libenter adiuvō eōs quī in perīculō sunt. | Semper aliquem libenter adiuvō in perīculō. | check phrase-order |
| 39 | and get out of this deadly flower bed as soon as you can. | et ex hōc flōrum lētālī agrō quam prīmum exī. / et ex hōc papāverum agrō mortiferō quam prīmum exī. | Et dē hōc lectō mortiferō flōris exī quam prīmum. | fix A3 A6 phrase-order fragment emoji |
| 41 | Oh, thank you! | Ō, grātiās tibi! | Ō, grātiās tibi agō! | ok|
| 43 | Oh, yes; you can save our friend, the Cowardly Lion, who is asleep in the poppy bed. | Ō ita; amīcum nostrum servāre potes, Leōnem Timidum, quī in agrō papāverum dormit. | Ō, ita; amīcum nostrum Leōnem Timidum servāre potes et quis in lectō dormit [poppy]. | fix A1 A8 emoji overflow |
| 46 | Why, it is said that he never lets anyone come into his presence. | Dīcunt eum nēminem umquam ad sē admittere. / Quid? Dīcitur nēminem umquam ad sē admittere. | Quid? Dīcitur numquam aliquem sinere. | check contact-relative |
| 47 | nor do I know of any living person who has seen him. | nec quemquam vīvum nōvī quī eum vīdit. | Neque hominem carbunculī quī eum vīdit sciō. | check A6 fragment |
| 50 | But to those who are not honest, or who approach him from curiosity, | Sed eīs quī nōn sunt honestī, aut quī cūriōsitāte ad eum accēdunt, / Sed eīs quī nōn probī sunt, aut quī cūriōsitāte ad eum adeunt, | Sed; aut quis eī ā cūriōsitāte appropinquat | check A7 fragment frame-fallback |
| 54 | Then he asked me what you looked like, | Tum mē rogāvit quālis essēs, / Tum mē rogāvit quālis vidērēris, | Tum mē rogāvit, | check A7 fragment |
| 55 | and he decided he would admit you to his presence. | et cōnstituit sē tē ad sē admissūrum esse. / et cōnstituit tē admittere. | Et cōnstituit sē tē ad praesentiam suam fatērī. | check fragment |
| 59 | I am only a Scarecrow, stuffed with straw. | Terriculum tantum sum, paleā plēnum. / Terriculum tantum sum, paleā fartum. | Ego paleā modo Terriculum sum. | check participle-phrase |
| 61 | I am a Cowardly Lion, afraid of everything. | Leō Timidus sum, omnia timēns. / Leō Timidus sum, quī omnia timeō. | Leō Timidus sum. | check A9 emoji |
| 62 | so that in reality I may become the King of Beasts, as men call me. | ut rē vērā Rēx Bēstiārum fīam, ut hominēs mē vocant. | Tam vēritās quam fortāsse fīō ut Rēx Bēstiārum quia virī mē vocant. | check A7 frame-fallback contact-relative purpose-guess fragment emoji |
| 65 | so get behind me and I will meet them as they come. | itaque post mē stā, et eīs venientibus occurram. / itaque post mē stāte, et eīs venientibus occurram. | Itaque accipe post mē et eōs nancīscar quia veniunt. | check fragment |
| 67 | You have called us for the third and last time. | Tertium et ultimum nōs vocāvistī. | Nōs tempus tertium vocāvistī. | check A7 |
| 69 | I can still make her my slave, for she does not know how to use her power. | Eam tamen servam meam facere possum, nam potestāte suā ūtī nescit. | Adhūc eam facere possum ut servus meus nam potestāte suā ūtī nescit. | check purpose-guess |
| 70 | I can starve you. | Famē tē necāre possum. / Tē famē cōnficere possum. | Tē [verb] possum. | fix A1 A9 missing-form |
| 71 | You are a wicked creature! | Mala bēstia es! / Scelesta es! | Bēstia mala es! | check|
| 72 | You have no right to take my shoe from me. | Nōn licet tibi calceum meum mihi auferre. / Iūs nōn habēs calceum meum mihi auferre. | Iūs quod calceum meum ā mē sūmit nōn habēs. | check noun-infinitive emoji |
| 74 | Well, in a few minutes I shall be all melted, | Bene, paucīs minūtīs tōta liquēscam, / Bene, mox tōta liquēscam, | Bene, paucīs corpusculīs omnīnō colliquefacta erō, | check A6 fragment |
| 76 | Are any of your people tinsmiths? | Suntne inter tuōs fabrī stannāriī? / Estne quisquam ē tuīs faber stannārius? | Estne [any]? | fix A1 unknown |
| 77 | and bend him back into shape again, and solder him together where he is broken? | et eum iterum in fōrmam reflectere, et ubi frāctus est cōnferrūmināre? / et eum rūrsus in fōrmam flectere, et ubi frāctus est coniungere? | Et inclīnatne eum in figūram retrō iterum et eum ūnā plumbat cum frāctus est? | fix A3 A4 A6 fragment |
| 78 | I’ll chop it down, and then we can get the Scarecrow’s clothes. | Eam caedam, et tum vestēs Terriculī capere poterimus. / Arborem caedam, et tum vestēs Terriculī sūmere possumus. | Eam caedam et tum pannōs Terriculī accipere possumus. | check|
| 80 | It seems to me I can scarcely wait till I get to Oz, | Vix exspectāre posse mihi videor dum ad Oz perveniam, / Vix exspectāre possum dum ad Oz perveniam, | Expectātiō mihi haud videt, | fix A4 A6 A7 fragment |
| 81 | and you must admit this is a very long journey. | et fatērī dēbēs hoc iter longissimum esse. | Et fatērī dēbēs hoc iter valdē longum esse. | check fragment |
| 82 | Why didn’t we think of that before? | Cūr id anteā nōn cōgitāvimus? | Cūr nōn anteā dē eō cōgitāvimus? | ok|
| 85 | Well, come to me tomorrow, for I must have time to think it over. | Bene, crās ad mē venī, nam tempus mihi est ut dē hōc cōgitem. / Bene, crās ad mē venī, nam tempus ad cōgitandum mihi opus est. | Bene, crās ad mē venī nam tempus eī super putāre dēbeō. | check|
| 86 | But don’t strike me—please don’t—and I’ll do anything you want me to. | Sed nōlī mē ferīre, quaesō, nōlī, et faciam quidquid vīs. | Sed nōlī mē pulsāre— quaesō nōlī pulsāre— et aliquid faciam et mē vīs. | check ellipsis |
| 88 | How was it that you appeared to me as a great Head? | Quōmodo mihi ut Caput magnum appāruistī? | Quōmodo mihi erat quam Caput magnum [appeared]? | fix A1 A7 emoji |
| 91 | I cannot tell you how to use them, however; you must find that out for yourself. | Tamen tibi dīcere nōn possum quōmodo eīs ūtāris; id ipsa invenīre dēbēs. / Tamen tibi dīcere nōn possum quōmodo eīs ūtendum sit; id ipsa invenīre dēbēs. | Tamen tibi dīcere nōn possum ut quam ūtar; id tibi cognōscere dēbēs. | fix A4 purpose-guess |
| 93 | Hereafter you will be a great man, for I have given you a lot of bran-new brains. | Posthāc vir magnus eris, nam multum cerebrī novī tibi dedī. / Posthāc vir magnus eris, nam cerebrum novum tibi dedī. | Posthāc vir magnus et dēlātor eris. | fix A3 A6 A7 participle-phrase emoji |
| 95 | Sit down, my dear; I think I have found the way to get you out of this country. | Sedē, mea cāra; putō mē viam invēnisse quā ex hāc terrā exeās. | Sedē, mea cāra; putō mē viam quae tē ex hāc terrā accipit invēnisse. | check speaker-gender noun-infinitive emoji |
| 97 | I have plenty of silk in the Palace, so it will be no trouble to make the balloon. | Multum sēricī in Rēgiā habeō, itaque facile erit follem facere. / Multum sēricī in Rēgiā habeō, itaque nūllus labor erit follem facere. | Cōpiam sēricī in Rēgiā habeō et tam opera quae follem facit nōn erō. | check noun-infinitive |
| 98 | I am now going away to make a visit. | Nunc abeō ut amīcum vīsam. / Nunc proficīscor ut aliquem vīsam. | Nunc abeō ut officium faciam. | check light-verb |
| 100 | I should like to cry a little because Oz is gone, | Paulum flēre velim, quia Oz abiit, | Flēre velim quia Oz paulum abiit, | check name-kept fragment |

### Proposed gold alternatives (for the main agent; the gold file is unchanged)
- #1 add "Itaque adhūc sagās et magōs inter nōs habēmus." - "have X among us" kept as habēre; same meaning.
- #4 add "Potesne mē adiuvāre ut viam meam inveniam?" - the gold keeps the possessive of the subject elsewhere
  ("Fābulam meam tibi nārrābō", "Rēgiam meam").
- #17 (matches now): "Nōn potes dēscendere?" - the rule is "nōn + verb first" for negated questions.
- #37 add "sed Cicōnia benigna mē servāvit," - order.adj puts a plain adjective after the noun.
- #41 add "Ō, grātiās tibi agō!" - the phrasebook's "thank you" (grātiās tibi agō) as in own_dialogue.
- #71 add "Bēstia mala es!" - order.adj.
- #81 add "et fatērī dēbēs hoc iter valdē longum esse." - own_dialogue renders "very" + adjective as valdē.
- #82 add "Cūr nōn anteā dē eō cōgitāvimus?" - "think of" is cōgitāre dē (verbprep_en_la.tsv); nōn after cūr as in
  #27 and #36.

### Decisions to confirm
1. Negated yes/no questions are said as statements with nōn first ("Nōn potes dēscendere?", "Nescīs?"), following the
   oz gold; DESIGN §10.3 says "don't you ..." -> nōnne. nōnne stays for tag questions.
2. "Then" in a statement is tum (oz #6, #54), in an imperative / question igitur (own_dialogue).
3. The fragment signal makes every cue that starts in lower case or ends with a comma Check.

### Review of quality loop 2 (main agent)
- Gold alternatives accepted into oz_sample.la.gold.txt: #1 (habēmus; the lexicon has sāga, -ae with a long ā, so
  "sāgās" is right and the gold's "sagae" was corrected to "sāgae"), #41 (grātiās tibi agō), #82 (cōgitāre dē).
  The other proposals (#4, #37, #71, #81) are correct Latin but their cues are Check, so the gold is left as it is.
- Decisions confirmed: (1) negated yes/no questions are rendered with nōn first ("Nōn potes dēscendere?"); nōnne is
  kept for tag questions only; DESIGN §10.3 is read that way. (2) "then" = tum in statements, igitur in imperatives
  and questions. (3) Every cue that starts in lower case or ends with a comma is Check (fragment signal).
- Remaining 50 mismatches go to quality loop 3 (C17); unknown words (lighted, braver, dented), witch singular/plural
  inconsistency, idioms, passives, bracketed stage text.

## Quality loop 3 (C17, 2026-10-06)
Material: the 47 remaining oz_sample mismatches (fidelity 2, speaker f; 53 / 100 at the start of C17 with the rebuilt
latin.vpl), tests/samples/sample.en.srt, own_dialogue EN / ES. tests/heldout/, tests/eval_gold/ and
data/work/eval/heldout-* were not opened. Generalisation guard: every rule below has at least two sentences of our own
in `engine/tests/test_rules_en.cpp` (test cases "rules-e: ..."), written before the rule was run on the sample; the oz
report is never the only evidence. The oz sample was used for tuning: the held-out number will be lower.

### Before / after
| file | start of C17 | end of C17 |
|---|---|---|
| oz_sample EN (100) | 53 / 100, wrong among OK 0 | **73 / 100** with 4 proposed gold alternatives (69 without), exact 48, ok 25 / check 67 / fix 8, wrong among OK 0 |
| own_dialogue EN (114) | 114 / 114, ok 66 / check 48 | 114 / 114, ok 65 / check 49 / fix 0 |
| own_dialogue ES (100) | 100 / 100, ok 78 / check 22 (after the latin.vpl swap) | 100 / 100, ok 81 / check 19 / fix 0 |
| sample.en.srt (12) | 12 / 12 | 12 / 12 |
| la2x own / orberg own / Greek regression | 201 / 201, 60 / 60, 114 / 114 | unchanged (201, 60, 114) |
| blind check (20 own sentences, children's dialogue) | first run **5 / 20** acceptable | 15 / 20 after the fixes below |

Measurement log (oz): 53 start; 55 participles / appositive phrases / tantum; 57 epithets, exclamations; 58 starve,
clothes, glosses; 59 future infinitive, presence; 61 what ... like, a little, particles; 63 for the third time, no
right to, home; 65 idioms (no trouble, new man, find my way), videor copula; 66 plenty of; 67 cleft questions, as +
noun; 68 so that, inner adverbial phrases; 69 noun adverbs next to their obliques, participle fragments; 73 with the
four proposed alternatives.

### What changed (C17)
- (a) Unknown and misread English forms (`src/frame/english.{h,cpp}`, `FrameBuilder::lemmaOf`, `Transfer::select`):
  irregular pasts the tagger read as presents (sang, swam) or as nouns (the bell rang) take Tense=Past from
  english.vpl; a word the lexicon knows only as an adjective is retagged (braver, clever after "a" or "too"); a past
  participle used as an adjective is the verb's participle (lighted -> accēnsus, sleeping -> dormiēns: `SemAdj::
  participle`, `LaAdj::participle`); a plural noun read as a verb before its own verb (Only witches wear ...); a noun
  read as a verb after a determiner (the clown fell); a verb hung under a noun is rebuilt as a flat clause (now with
  possessives 's, prepositional groups and a fronted time group: "Every morning I walk ..."); a form english.vpl does
  not list and a lemma the reverse index does not list are derived from base words before giving up (hyphenated:
  joined, then the head "bran-new" -> new, "sea-shore" -> lītus; regular -ed/-ing/-s/-es/-ies/-ied/-er/-est/-ly with
  doubled consonants, y -> i, a dropped e; endings are restricted to the part of speech); "-ly" adverbs go through
  the adjective; plural-only nouns keep their lemma (clothes -> vestēs); "its" is reflexive suus.
- (b) Consistency: `transfer::Memory::nounSense` (32 entries, oldest dropped) keeps an English noun's Latin word for
  the rest of the batch when that word is one of the candidates (witch stays sāga); forced alternatives bypass it.
- (c) Idioms and light verbs: states_en_la.tsv "verb noun" rows with a fixed object (make a visit -> aliquem vīsō,
  take a walk, have a rest, make a mistake (an event: perfect), make a noise, give a shout, have a swim, have a
  dream); phrasebook rows (starve -> famē cōnficere "(transitive)" only with an object; no right to {VP}; no trouble
  to {VP}; in a few minutes / a minute / a moment -> mox; I feel like a new man; find my way -> viam invenīre; at all;
  hush; before dark); verbprep rows (admit to/into, take from + person = dative of separation (frame dat), bark at);
  phrasal rows (walk / go / move / carry on -> pergō); "into his presence" -> ad sē, "in my presence" -> cōram mē;
  "go home" -> domum, "at home" -> domī, "from home" -> domō; "plenty of / a lot of" -> multum + genitive, "lots
  of" + plural -> multī; "all" + a singular noun -> tōtus (all night -> tōtam noctem); "for the third (and last)
  time" -> tertium (et ultimum), first -> prīmum; "a little" after a verb -> paulum; aspect particles (on, off, out,
  over ...) without a phrasal row are not translated through the reverse index ("walked on" was "aͣ"); "so badly",
  "very slowly" -> both words; adverbial phrases inside or at the end of a clause (of course, in fact, at last, at
  once) go inside it (after its conjunction, or before its final verb group); "how/why was it that ..." clefts; "men"
  in general -> hominēs; "as" + noun -> ut + nominative; exclamations put the adjective of a predicate noun first
  (Mala bēstia es!); a lower-case epithet before a definite title or name stands before it (benigna Cicōnia, fortis
  Rēgīna, vetus Grumbo); a person noun predicate of a feminine subject takes its feminine (puella, magistra, serva).
- (d) Participles: object complements of factitive verbs (make / call / name / elect / keep ...: "eum rēgem fēcērunt",
  "mē fortiōrem nōn facit", "puellam amīcam suam fēcit"); participle / adjective phrases after a comma
  (`SubRel::Apposition`: "Terriculum tantum sum, paleā plēnum", "Leō Timidus sum, omnia timēns", "pulvere tēcta");
  a clause fragment "or the Tin Woodman badly dented on the rocks below" is the noun with a perfect participle (a
  transitive -ed verb, an English participle-only form, or an agent "by"), the participle last, "badly" -> graviter,
  the agent ā/ab + ablative for persons; "would" in reported speech -> future infinitive agreeing with its subject
  (sē ventūrās esse); a 3rd-person pronoun after a noun subject of saying is reflexive.
- (e) Purpose and complements: wh + to-infinitive is an indirect question with the person of the one told (quōmodo
  eīs ūtāris, quō eat, quid facerem; after teach / know the infinitive stays); "what X looks like" -> quālis; "so
  that" -> ut + subjunctive (also a clause cut from its sentence), no fortasse for "may"; after an imperfect or
  pluperfect the purpose clause is in the imperfect subjunctive; a to-infinitive hung on the goal of a verb of motion
  is purpose ("went to the river to wash"); "going to the market to buy" is motion, not the future; "become / remain
  / seem" + noun / adjective -> predicate (Rēx fīam); an adjective as the object of have / wear / like -> substantive
  neuter (album habēs); "tell X" (person) -> dative; "as" after its clause -> ut + indicative (ut hominēs mē vocant);
  ", so ..." joins with itaque alone; relative "where" -> in quā (live -> habitō).
- (f) Fragments: the units above keep fragments grammatical (participle fragments, "so that" fragments, connectors).
- (g) Square brackets: editorial text inside an unfinished sentence ("They are rusted [so badly] that ...": lower
  case, two words or more) stays in the sentence; the brackets go back around the Latin of those words when they are
  contiguous, else none; flag `editorial` (Check). A bracket group after a finished sentence or a one-word sound
  stays a nonverbal piece as before.
- (h) Names: a capitalised word english.vpl does not know (nor a base of it), inside the sentence or before a verb,
  is a name (kept, Check, never Fix); names keep their adjectives, relative clauses and coordination ("Flimsy et
  Grub", "Terriculum et Leō"); two subjects joined by "and" that the parser hung on the verb are conjuncts.
- Checker (A3 / A4 false alarms that made correct Latin Fix): adverbs chosen by the generator (tantum, quō, īnfrā,
  coordinated "tertium et ultimum") are not checked as adjectives / prepositions / relatives; finite verbs chosen by
  the generator (habitō) are verbs; a periphrastic infinitive's participle agrees with the accusative subject;
  videor / fīō are copulas; partitive "multum aquae"; an indirect question after its verb and a relative after its
  preposition start a segment; cum + nominative with a later finite verb is the conjunction; "et" between two
  nominatives (also names) is a plural subject; an infinitive object licensed by valency (scio inf) is not the
  ablative of a homograph.
- Coordinator add-on (lemma choice): lemmas with the same cleaned headword, part of speech and principal parts
  (genitive / infinitive / perfect) are one candidate: the best score represents them (on equal scores the lower
  tier, then more paradigm cells, then the lower id) and the others are dropped, so they never count as a competitor
  for the margin ("¡Mira el cielo!" -> "Spectā caelum!" OK again; ES ok 81). Deviation from the requested rule: the
  winner is chosen by score first (the tier first would let caelum "chisel", tier 1, stand for "sky"; the text is
  the same either way) and the principal parts must match, so homographs that inflect differently stay apart (volō
  "want" / volō "fly"). Two-gender nouns (caelum MN) agree as neuter in the singular.
- CLI (C8b hand-off): words.list prints `morph::cleanHead(head)`.

Rows added: tiers_la.tsv 6 (captīvus, appāreō, contundō, culīna, capra, saliō); phrasebook_en_la.tsv 18 (starve,
6 no right to, 3 no trouble to, 3 in a few minutes / minute / moment, new man, find my way, at all, hush, before dark;
the "new man" row sits before "i feel like {NP}" so it wins the tie); states_en_la.tsv 10 light verbs; verbprep 4
(admit to, admit into, take from (dat), bark at); phrasal 4 (walk / go / move / carry on); valency_la.tsv 1 (contundo
acc) and scio + inf.

API changes (additive, public headers): frame.h `SemAdj::participle`, `SemFrame::objComplement / objComplementAdj /
secondary`; realise_la.h `LaAdj::participle / coord`, `LaNP::indefinite`, `LaClause::objPredicative / objPredAdj`,
`SubRel::Apposition` (appended), `LatinRealiser::ClauseCtx::apposition`; transfer.h `Memory::nounSense`, private
`adjectiveInto`, `feminineOf`. curated: verbprep frame "dat". Changed test expectation: "Can you help me find my way?"
-> "Potesne mē adiuvāre ut viam inveniam?" (phrasebook vp row, as oz gold #4); the oz threshold is 68.

### Remaining oz mismatches (fidelity 2)
Free renderings the rules cannot derive (#5, #13, #23, #30, #34, #80, #85, #86, #93, #95), lexical / idiom choices of
the gold (#2 potēns / "all the rest of us", #21 match -> fax, #39 / #43 flower / poppy bed -> ager papāverum, #74 tōta
liquēscam, #76 tinsmiths, #77 solder, #81 longissimum), word order of heavy NPs (#12 eī quem ... after the verb, #38),
"for yourself" -> ipsa (#91, addressee gender), "them" for unnamed things (#26 eōs / ea), "lets anyone come into his presence" (#46), parser failures (#14 the
coordination "witches and sorceresses", #47, #50, #65).

### Proposed gold alternatives (for the main agent to accept or veto; added to oz_sample.la.gold.txt after " | ")
- #33 "sed id mē fortiōrem nōn facit," - "that" is id in our rules (as the gold's #82 "Cūr id anteā ...").
- #69 "Adhūc eam servam meam facere possum, nam potestāte suā ūtī nescit." - "still" read as time (adhūc); the
  English is ambiguous between "even now" and "nevertheless" (tamen).
- #78 "Eam caedam, et tum vestēs Terriculī accipere possumus." - accipere "get, receive"; the cue is Check
  (light-verb: "get" + a thing is never OK).
- #98 "Nunc abeō ut aliquem vīsam." - the gold's two alternatives combined (abeō of #1, aliquem of #2).

### Blind check
20 sentences of children's dialogue written at the start of C17 (before any change; never looked at until the end).
First run: 5 / 20 acceptable (5, 9, 13, 17, 20). Faults found: "going to the market to buy" as a future (an OK cue that
was wrong), tense sequence after an imperfect, "tell my brother" (accusative), "et itaque", "too clever" (unknown),
"as big as this one", the clown tagged as a verb, "barked at ... all night" (no clause), "its" as eius, hop ->
circumsiliō, kitchen -> hortus, goat -> hircus, hush, before dark. After the fixes (each with own test sentences):
15 / 20. Still wrong: #2 the fair (pulcher), #3 hiding (cēlās without object), #11 "was frozen" (gelābat), #12 "as
big as this one", #18 "left the gate open" (posuit).

### Review of quality loop 3 (main agent)
- Gold alternatives accepted: #33 ("that" as id), #98 (abeō ut aliquem vīsam). Vetoed: #69, where "I can still make
  her my slave" is concessive (tamen), not temporal (adhūc); #78, where "get the clothes" is fetching (capere, sūmere),
  not receiving (accipere). oz_sample therefore stands at 71/100 with wrong among OK 0.
- Blind check honesty noted: 5/20 acceptable at first run, 15/20 after fixes. The first-run figure is the one that
  predicts held-out behaviour; see STATUS E3 for the measurement.

## Quality loop 4 (C19, 2026-10-07)
Material: the remaining oz_sample mismatches (C17 table), the held-out error classes of E3 (flags and checks only, no
text: fragment 63, unknown 28, emoji 24, purpose-guess 20, missing-form 10 ...), four frame cases from the Greek
review (coordinator), tests/samples/sample.en.srt, own_dialogue EN / ES. tests/heldout/, tests/eval_gold/ and
data/work/eval/heldout-* were not opened. Generalisation guard: every rule below has at least two sentences of our own
in `engine/tests/test_rules_en.cpp` (test cases "rules-f: ..."), written before the rule was run on the oz sample;
20 blind sentences of children's dialogue were written at the start (before any change) and run once at the end.

### Before / after
| file | start of C19 | end of C19 |
|---|---|---|
| oz_sample EN (100) | 71 / 100, exact 48, ok 25 / check 67 / fix 8, wrong among OK 0 | **74 / 100** without and **78 / 100** with the 4 proposed gold alternatives below; exact 50; ok 26 / check 68 / fix 6; wrong among OK 0 |
| own_dialogue EN (114) | 114 / 114, ok 65 / check 49 | 114 / 114, ok 65 / check 49 / fix 0 |
| own_dialogue ES (100) | 100 / 100, ok 81 / check 19 | 100 / 100, ok 81 / check 19 |
| sample.en.srt (12) | 12 / 12 | 12 / 12 |
| la2x own / orberg own | 201 / 201, 60 / 60 | unchanged |
| Greek: EN->GRC / ES->GRC / GRC->EN, ES | 114 / 114, 38 / 40 (39 / 40 with C18's work), 40 / 40 | unchanged (114, 39, 40) |
| blind check (20 own sentences) | first run **10 / 20** acceptable | 14 / 20 after the fixes below |

The oz threshold in the test is raised 68 -> 74 (the count without the proposed alternatives).

### (a) emoji on wrong cues
Finding: the `emoji` flag never changes confidence and the emoji is not part of the target text (RealiseOptions::
emojiInText is false; A1-A9 never see it), so it cannot make a cue wrong. It is a passive marker of a depictable noun:
on our material it sits on 28 / 100 oz cues and 42 / 114 own_dialogue cues, and 4 / 8 oz Fix cues; 24 / 132 (18 %)
of the held-out wrong cues is below that base rate. The 24 are wrong for their other flags (fragments, unknown words,
parse errors), which the items below address. One real fault was found and fixed: translated titles and names got
an emoji ("Leō🦁 Timidus", "Cicōnia", DESIGN 10.3 says never for names): `LaNP::capitalise` / `nameWords` heads take
none now (test "rules-f: emoji never on a title ...").

### (b) missing-form
- `Engine::retryMissingForms`: a token with "no lexicon cell for these features" makes the sentence be translated again
  with the next candidate of that word forced (at most three tries; the first result without a missing form wins), so
  a defective lemma never ends as a bracketed Fix literal while another candidate has the form.
- A Latin verb without passive forms says the English passive actively: "I shall be all melted" -> liquēscam,
  "The witch was melted" -> Sāga licuit (passive probe on the 3rd plural; impersonal 3rd singular cells do not count).
- A personal passive takes a transitive candidate (accusative valency, transitive sense tag, passive cells) when one
  scores within 0.6: "The boy was hurt" -> laesus est (not doleō), "The ship was damaged" -> afflīctāta (not noceō +
  dat, which made "nocita est" OK and wrong before).
- The paradigm generator stays reserved for lemmas without a table (DESIGN 6); with a table the next candidate is used.

### (c) unknown words (src/frame/english.{h,cpp}, transfer)
- Compounds the lexicon lacks: `en::compoundParts` splits a known first word and a head of a closed list (-man/-men,
  -woman, -maid, -boy, -girl, -smith, -keeper, -maker, -castle, -house, -folk, -bird, -cake, -ball, -fly, -worm,
  -room, -yard, -tree, -berry, -stone, -pot): head noun + genitive of the first word ("snowman" -> vir nivis,
  "milkmaid" -> ancilla lactis, "sandcastle" -> castrum harēnae), flag `derived-word` (Check). Never "kit" + "ten".
- Numerals in words: 13-19, 40-90, hyphenated 21-99 ("twenty-two cows" -> vīgintī duōs bovēs: the units word agrees);
  any other number in words is written in Roman numerals ("XXI ovēs"; A1 accepts Roman numerals) instead of being
  dropped (a dropped numeral was wrong and OK).
- Contractions by the next word: 'd + past participle / "better" = had ("He'd seen" -> vīderat; "You'd better go" ->
  should -> dēbēs), 's + got / been = has, "have got" (possession) = have ("I've got a new hat" -> Pilleum novum
  habeō); table rows y'know, 'tis, 'twas, gimme, lemme; "let me" -> sine mē + infinitive (phrasebook), "let him go"
  -> Eat (jussive of the subject's person, was "Eāmus").
- Interjections: hurrah / hurray / yippee -> iō, bravo -> euge, aha -> ā (tier rows iō, euge).
- Proper adjectives before a noun are adjectives, never dropped ("a Roman soldier" -> mīles Rōmānus; "Mīles est" was
  wrong and OK); a noun used as a modifier with no Latin adjective is a genitive ("the poppy bed" -> ager papāverum).
- An adverb in -ly without a Latin adverb in the reverse index takes the Latin adjective's own adverb when the lexicon
  lists it (clārus -> clārē, fortis -> fortiter, dulcis -> dulciter); never an invented form.

### (d) fragments
- A lower-case cue that starts with a preposition or "and / or / but / nor" was tagged as an imperative verb
  ("with a loud cry." -> "Clāmōrem magnum [with]", "to the hungry birds." -> "Cursitā avēs"): such words before a noun
  phrase are retagged (the verbless-sentence retag no longer turns them back into verbs).
- A prepositional-phrase fragment keeps its preposition's case: an oblique of the fragment ("Sub mēnsā magnā,",
  "In silvam obscūram,", "Ad domum parvam.", "Clāmōre magnō."); a noun-phrase fragment is nominative; a possessive in a
  fragment or inside the subject is eius (never a reflexive without a clause subject: "Rēx et fīliī eius").
- Continuation: when the previous sentence of the same speaker ended open (, ; : dash) a fragment "and / or + noun
  phrase" takes the case of that sentence's object ("I saw the king," + "and the queen." -> "Et rēgīnam.";
  `Memory::lastObjCase / contCase`).
- A noun phrase with a prepositional modifier is a fragment, not a failed parse ("a very small mouse with a long tail."
  was could-not-parse).
- "those who ..." -> eīs quī ...; a cue-initial coordinator parsed as the root gives the root back to its phrase ("But
  to those who are honest," -> Sed eīs quī honestī sunt,); a second relative coordinated with the first keeps the
  relative pronoun ("aut quī ... appropinquant", not "aut quis"); "from" + a feeling is a bare ablative of cause
  (cūriōsitāte, timōre; A4 accepts it with a verb that takes the accusative); a PP fragment whose verbs are all inside
  its relative clauses is not a troubled parse.

### (e) A6 tier
The fidelity-2 weights were checked: -0.25 per tier above 2, +0.1 for tier 1, the teacher gloss +0.5, and the C2b
"tier preference" (a tier 1/2 candidate with the sense keyword and base >= max(0.2, half the top) beats a tier 3 top).
On all our material (oz, own, the probes of this loop) only four tier-3 choices had a tier 1/2 candidate at all
(sorceress -> sāga base 60, deadly -> āter, ladder -> gradus "gloss modifier only", possible -> potis), none of the same
sense: the scorer is right there. The A6 findings come from words whose every Latin candidate is tier 3 and from
closed-class tables choosing tier-3 words. Tier rows (each with two own sentences, test "rules-f: core words ..."):
serva, avia, coquus, baculum, uxor (tier 1); graviter, retrō, tertium, argenteus, ubīque, nusquam, usquam, rāna, gigās,
glaciēs, catulus, papāver, fūnis, pilleus, liquēscō, occurrō, iō, euge and the numerals ūndecim .. ūndēvīgintī,
vīgintī .. nōnāgintā, ūndecimus, duodecimus, vīcēsimus, trīcēsimus, centēsimus, mīllēsimus (tier 2); Rōmānus (tier 1).

### (f) structure, oz items and the frame cases of the Greek review
- Relative clauses attach to their noun: after a comma ("my teacher, who is very kind" was "et quis ...", OK and
  wrong), right after a noun when the parser hung them on the verb ("people who are honest" was an accusative +
  infinitive, OK and wrong), a "who" clause hung on another noun of the phrase ("bread to the man who was hungry").
- "a person who ..." / "people who ..." -> is quī; an object "is quī ..." follows the verb group with the modal before
  its infinitive (#12 "nēmō audēbit nocēre eī quem ..."); a noun with its relative clause stays before the verb (the
  realiser table and own_dialogue).
- An apposition to the object between commas has the object's case and follows the verb when it carries a relative
  clause (#43 "amīcum nostrum servāre potes, Leōnem Timidum, quī in agrō papāverum dormit"); "X bed" of plants ->
  ager + genitive plural; "out of" -> ex.
- Copula + prepositional phrase is a place ("She is in the garden." was "Hortus est.", OK and wrong; "We have been to
  Rome." -> Rōmae fuimus); "Where were you? / Where have you been?" (where read as a subject, been as a clause) ->
  Ubi erās? / Ubi fuistī?; a word after a possessive is a noun ("Where is my hat?" was "Ubi meum ferit?"); a predicate
  after "be" that the lexicon knows as an adjective is one ("a person who is kind" -> benignus, not genus).
- "as + clause" about the object -> the object's present participle (#65 "eīs venientibus occurram"); "get behind" ->
  stō, "get under" -> subeō (verbprep rows); occurrō + dative for "meet"; "for / by + yourself, myself ..." -> emphatic
  ipse agreeing with the subject (#91; gender of the person addressed from the speaker glossary, flag speaker-gender);
  "all" describing the subject of a passive or of an adjective -> tōtus / omnēs (#74 "mox tōta liquēscam"); "get" + a
  thing (not in the past, not from a person) -> capiō; "can" joined to a future clause -> future (#78 "capere
  poterimus"); pronoun + "all" as object keeps the pronoun ("nōs omnēs"); eīs for the dative plural of is (macron
  override, as iīs was listed first), trēs / omnēs for the accusative (trīs / omnīs).
- Greek-review cases (coordinator): (1) "When / Before / After / While / As soon as X, Y." is a statement with a time
  clause: the subordinate verb after its subject is retagged within its comma segment (rose = rise, set = set),
  a parse that made the subordinate verb the root is turned round, "as soon as" -> ubi; sun / moon / stars rise ->
  orior, set -> occidō; "sett" is no lemma of "set". (2) "The shepherd, seeing the wolf, fled." -> "Pāstor, lupum
  vidēns, fūgit.": rebuilt on the Latin side (Transfer::clause), not in the frame: a frame-level repair made C18's
  committed Greek review test lose the participle (the Greek transfer ignores SemFrame::secondary with an object), so
  the frame keeps the parser's shape and each language rebuilds it; LaSub::afterSubject places the phrase between
  commas; flee -> fugiō (the "flee" gloss moved from aufugiō to fugiō). (3) A possessive the parser hung outside a
  phrasebook slot is kept ("Ubi est māter tua?"), and content words of a slot count for A7 only when the slot's
  translation used them. (4) A sentence-initial verb before a comma is an imperative, never a name candidate ("Hurry,"
  "Run,"); a common noun there is the person addressed ("Grandmother," -> Avia); imperative phrasebook rows need the
  bare verb ("She woke up." is no "ēvigilā").
- Blind-check fixes: a possessor 's hung on the verb ("The fisherman's wife" was "Piscātōrem uxor"); give / show /
  bring + person + thing parsed as one phrase (indirect object); "may I / we ...?" asks permission (licetne mihi /
  nōbīs + infinitive; "Mother, may I go out?" was "exeōne fortāsse", OK and wrong); an -s verb after "where the X"
  (lives, not the plural of life) and live = dwell (habitō) with where / here / there; "than the first" agrees with
  the subject (quam prīmus); a plural noun subject coordinated by "and" before a bare verb ("kings and queens wear");
  "witches and sorceresses" parsed as compound + "and" is a coordination; "two-gender" nouns with a plural in -a agree
  as neuters (papāvera rubra).
- Checker false alarms fixed: "quam prīmum", emphatic nominative ipse, a nominative participle closing a phrase between
  commas, a neuter substantive object right before its verb (only when no accusative noun stands next to it; the
  corruption test stays 200 / 200), ablatives of cause.

Rows: tiers_la.tsv +47 (listed under (e), plus uxor) and 3 notes edited (coquō "cook, bake", fugiō "flee", aufugiō
"run away, escape"); phrasebook_en_la.tsv +3 (let me {VP}, how have you been, how old are you); phrasal_en_la.tsv +1
(hide: cēlō|lateō); verbprep_en_la.tsv +2 (get behind -> stō, get under -> subeō); contractions_en.tsv +5; macron_
overrides.tsv +3 (omnis omnīs -> omnēs, tres trīs -> trēs, is iīs -> eīs).

API changes (additive): transfer.h `Memory::lastObjCase / contCase`; realise_la.h `LaNP::numeralLiteral`,
`LaAdj::after`, `LaSub::afterSubject`; frame.h private `FrameBuilder::contractionContext`; english.h
`en::compoundParts`; src/transfer/tables.h `plantNoun`, `celestialNoun`. New flag `derived-word` (Check).
Changed test expectation: C17 "Only witches wear black hats." -> pilleōs (tier row pilleus "hat, cap").

### Proposed gold alternatives (added after " | " in oz_sample.la.gold.txt; veto freely)
- #39 "et ex hōc agrō mortiferō flōrum exī quam prīmum." - the gold's words, the genitive after the noun (order.gen) and
  the tail phrase after the verb.
- #50 "Sed eīs quī honestī nōn sunt, aut quī eī cūriōsitāte appropinquant," - appropinquāre + dative is "approach".
- #65 "itaque stā post mē, et eīs venientibus occurram." - order.imp: a short imperative puts the verb first.
- #91 "Tamen tibi dīcere nōn possum quōmodo eīs ūtāris; id ipsa cognōscere dēbēs." - "find out" = cognōscere
  (phrasal_en_la.tsv).

### Blind check
20 sentences of children's dialogue written at the start of C19 (before any change), run once after the changes:
**10 / 20** acceptable at the first run (2, 3, 4, 5, 6, 7, 8, 9, 11, 20). Wrong: #1 "Grandmother, may we bake ..."
(name guess, fortāsse, nātālis), #10 "out of wood" (ē silvā), #12 "brightly" unknown and "tonight" -> hodiē, #13 "left
the door open", #14 "one day" (ūnō diē for "some day"), #15 "than the first" (prīmum), #16 "gets dark", #17 "gave the
poor old man some bread" (accusative + genitives), #18 "where the dragon lives" (lives as a noun), #19 "the
fisherman's wife" (Piscātōrem). After the fixes above (each with own test sentences): 14 / 20; still wrong #1
(nātālis agreement, torreō/coquō aside), #10, #12 (tonight), #13, #14, #16. Note: the hide row (cēlō|lateō) came from
C17's list, not from this blind set, but blind #2 uses it.

### Open points
- C18's committed test `rules-grc3: C18 review` expected "Hurry, the ship is leaving!" in Greek to be Check because the
  frame read "Hurry" as a name. With the requested frame fix (case 4) the frame now records the repair ("retag": a
  sentence-initial word the tagger took for a name and the lexicon gives back as a verb or a common noun), so the cue
  stays Check in both languages and that test passes (13 / 13 Greek review cases).
- "Only kings and queens wear crowns.": "only" is lost in the rebuilt clause (A7 Check); "only" + subject -> sōlus is
  not done.

### Review of quality loop 4 (main agent, 2026-10-07)
- Gold alternatives #39, #50, #65, #91 accepted (same meaning, correct Latin): oz_sample 78/100.
- Fresh spot check, 15 unseen sentences, fidelity 2: OK 5 (4 right; "Run, the dragon is coming!" -> "Curre et dracō
  venit!" turns the comma into et and is still OK: a fault), Check 9, Fix 1 ("taller than yours" -> [yours]).
- For loop 5 (C20, with the acceptance file): standalone possessive pronouns (yours, mine, his) -> tuus/meus/eius
  agreeing with the compared noun; an imperative followed by a comma and a clause keeps the comma (asyndeton), never
  et; "such a big" -> tam magnus / tantus; "so hungry that" -> tam ēsuriēbāmus ut (tam was dropped); "a book to
  read" -> librum legendum / ad legendum, not quī legit.

## Pre-loop C20 (2026-10-07)
Material: the five faults of the main agent's review of loop 4 (standalone possessives, imperative + comma, such / so,
noun + to-infinitive, "my child"), the speed regression of E4, own sentences, tests/regression/oz_sample.en.srt,
own_dialogue EN / ES, tests/samples/sample.en.srt. tests/heldout/, tests/eval_gold/, data/work/eval/heldout-* and
data/acceptance/ were not opened. Generalisation guard: every rule below has at least two sentences of our own in
`engine/tests/test_rules_en.cpp` (test cases "rules-g: ..."), written at the start (before any change) and before the
rule was run on the oz sample; 20 blind sentences of children's dialogue were written first (03:20 UTC) and run once
after the changes.

### Before / after
| file | start of C20 | end of C20 |
|---|---|---|
| oz_sample EN (100) | 78 / 100, exact 50, ok 26 / check 68 / fix 6, wrong among OK 0 | 78 / 100 (unchanged; #23 and #47 changed but stay Check and unmatched), exact 50, ok 26 / check 68 / fix 6, wrong among OK 0 |
| own_dialogue EN (114) | 114 / 114, ok 65 | 114 / 114, ok 65 (report byte-identical) |
| own_dialogue ES (100) | 100 / 100, ok 81 | 100 / 100, ok 81 (report byte-identical) |
| sample.en.srt (12), la2x 201, orberg 60 | 12, 201, 60 | unchanged |
| Greek EN->GRC / ES->GRC / GRC->EN, ES / C18 review | 114 / 114, 39 / 40, 40 / 40, 13 / 13 | unchanged (reports byte-identical; also checked on HEAD + the C20 files only) |
| 800 cues (proxy below), test harness | 24.4 s | 2.4 s (speed fix alone, output byte-identical), 2.6 s at the end of C20 |
| blind check (20 own sentences) | first run **13 / 20** acceptable | 17 / 20 after the fixes below |

No gold alternatives are proposed (no oz output became a correct non-matching rendering).

### (6) Speed (done first, output unchanged)
Profile: valgrind callgrind (no new dependency; perf is not installed) of one run of the 100 oz cues through the
`rules-f: try` harness: 23.4 G instructions, 88 % inside `Transfer::select`, of which `text::nfc` 64 % and
`morph::parsePrincipal` 37 %. Cause: the C17 coordinator add-on (lemmas with the same cleaned head, part of speech and
principal parts are one candidate) built the signature of candidate j (cleanHead + parsePrincipal + four NFC
normalisations) inside the inner loop of a pairwise comparison, i.e. O(n²) signatures per `select` call, and `select`
runs for every content word, every derived base form, every missing-form retry and every two-part analysis. So the
missing-form retries and two-part analyses of C19 were not slow in themselves: they multiplied the calls of a quadratic
`select` (C17 25.8 s -> C19 37.5 s in E4). Fix: each signature is computed once per candidate (a vector beside the
candidates); the comparison is unchanged. After: 3.3 G instructions for the same run, of which about two thirds is
loading the tagger / parser models (SHA-256 of the .vpt files, twice in the test harness, not per cue); translation
about 1.1 G. Timings on this machine (Release, test harness, incl. about 0.25 s of loading): oz 100 cues 3.61 s ->
0.48 s; a proxy of 800 cues (oz_sample x 7 + 100 own_dialogue cues; the held-out files were not opened) 24.36 s ->
2.36 s. Output check: VP_RULES_TRY dumps of oz, own_dialogue and the 800-cue proxy and all seven regression reports
(EN, ES, oz, GRC, GRC ES, grc2x, la2x back-translation) compared with cmp before / after: byte-identical; the
determinism tests pass. Expected E4 cost now: a few seconds for 800 cues (target < 30 s).

### (1) Standalone possessive pronouns
`Transfer::npInto`: mine / yours / ours / his / hers / theirs as a noun phrase of their own stand for a noun said
before them: meus / tuus (vester for a plural addressee) / noster agreeing with that noun, the noun left out ("My
brother is taller than yours." -> "Frāter meus altior est quam tuus.", "Domus tua maior est quam mea."); third person
eius / eōrum ("The red ball is his." -> "Pila rubra eius est.", "quam eōrum"). The noun is the compared one after
"than" (the subject of a predicate adjective, else the object, whose case the quam phrase then takes: "quam meum"),
else the last noun mentioned (Memory::lastGender / lastNumber: "I lost my pen, can I use yours?" -> tuō ūtī). Never a
bracket.

### (2) Imperative, comma, statement
An imperative followed by a comma and a statement with its own subject (parataxis, or a complement without "that" the
parser made of it) is asyndeton: the comma stays, no et, no accusative + infinitive (`LaSub::asyndeton`): "Run, the
dragon is coming!" -> "Curre, dracō venit!" (OK: nothing was repaired), "Come quickly, the bread is burning!" -> "Venī
celeriter, pānis ārdet!". A real "and" keeps et ("Curre et latē!"). Frame (english.cpp): a sentence-initial word before
a comma that the tagger took for an interjection and that the lexicon knows as a verb but not as an interjection is
the imperative ("Hide, the witch is here!" lost "Hide" and was OK; now "Latē, sāga hīc est!", Check: repair "retag").

### (3) such / so
"such a" + adjective -> tam + adjective, before the noun ("Tam pulcher diēs erat."); "such a big / great X" ->
tantus ("Numquam tantum canem vīdī."); "such a" + noun alone -> tālis ("tālem strepitum"); "so many" -> tot, "so
much" -> tantus. "such" was covered silently (OK with the word dropped). A state adjective said by a verb keeps its
degree words ("so hungry" -> tam ēsuriēbāmus, "very afraid" -> valdē timeō). "so ... that": the result clause also
after "so fast" and after a such / so-many noun; a "that" clause the parser hung on such a noun as a relative clause
with its own subject is the result clause ("She has so many friends that she is never alone." -> "Tot amīcōs habet ut
numquam sōla sit."; it was "quōs numquam sōla est habet", OK and wrong; rebuilt in `Transfer::clause`). Sequence of
tenses in result clauses: after an imperfect / pluperfect the imperfect subjunctive ("Tam ēsuriēbāmus ut omnia
ederēmus"); after a perfect the perfect subjunctive of an actual result stays ("Tam male cecinit ut rīserīmus", C17)
and a present becomes imperfect.

### (4) Noun + to-infinitive
`Transfer::relativeInto`: a bare to-infinitive on a noun is not a relative clause: the gerundive agreeing with the noun
when the Latin verb's chosen sense is transitive and the noun is its object ("Dā mihi librum legendum.", "Epistulam
scrībendam habeō.", "Aquam bibendam nōn habēbant."), else ad + the gerund ("Locum ad dormiendum", "Pānem ad
edendum", "aliquid ad edendum" for something / anything / nothing). New `LaAdj::gerundive`, `LaNP::adGerund`. The
frame doubt noun-infinitive stays (Check): the relative clause of purpose (librum quem legam) is the other reading.

### (5) "my child" in address
"my child" / "my kid" addressed -> mī fīlī / mea fīlia (by the main character's gender, as "child" -> puer / puella;
the possessive first); "my" was dropped silently on an OK cue. "child" alone keeps puer / puella.

### (7) Blind check
20 sentences of children's dialogue written at the start of C20, run once after items 1-6: **13 / 20** acceptable at
the first run (1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 15, 20; 8 "eum" for the fox where a teacher would write eam is
counted acceptable, 12 prōmptum for "ready" is borderline). Several blind sentences touch this loop's items (7, 8, 13,
20), which favours the count. Wrong: #6 "the lost kitten" -> vapidum, #9 "something to eat" (dropped), #14 "all
afternoon" (genitive tōtius vesperī), #16 "three times" -> tribus temporibus and "knock on" -> in iānuā, #17 "Whose
shoes are these?", #18 "until the moon rose" (a rose), #19 "a present" -> praesentiam. Fixes (each with own test
sentences, "rules-g: fixes after the blind check"): numeral adverbs for "N times" (bis, ter, quater ... deciēns,
centiēns, mīlliēs; "many times" saepe, "several times" aliquotiēns; twice -> bis in the adverb table); verbprep rows
knock on / at -> pulsō + object; a past form closing a sentence after "until / till / when / before / after / while /
since + noun" is that clause's verb (english.cpp), "until" + a past event -> dōnec + perfect indicative (dum otherwise);
something / anything / nothing + to-infinitive -> ad + gerund; dōnum note "gift, present" (teacher gloss); an
adjective that is also a verb's past participle with only a weak Latin adjective takes the verb's perfect participle
(lost -> āmissus with the teacher row āmittō "lose"), also when tagged as a finite verb before its noun ("the lost boy")
or after a possessive ("her lost ring" was "Invēnit sē ānulum perdidisse"); a deponent is never the passive participle
("the stolen crown" was fūrātam). After: **17 / 20**. Still wrong: #14 (the parser hangs "all afternoon" on "garden"),
#17 (whose -> cuius and a wh question), #19 ("get" = receive is capiō since C19). Found while probing and fixed: the
perfect passive infinitive of a reported statement agrees with its accusative subject ("Sciō portam clausam esse",
was clausum: A3 Fix), and an object pronoun of a reported statement that is the speaker is the reflexive ("He said that
the girl had followed him." -> "Dīxit puellam sē secūtam esse"; eum was OK and wrong).

Rows: tiers_la.tsv +14 (bis, ter, quater, quīnquiēs, sexiēs, septiēs, octiēs, noviēs, deciēns, centiēns, mīlliēs,
aliquotiēns, dōnec tier 2; āmittō tier 2 "lose, let slip, let go") and 1 note (dōnum "gift, present");
verbprep_en_la.tsv +2 (knock on, knock at); transfer/tables.cpp adverbs twice -> bis, thrice -> ter.

API changes (additive): realise_la.h `LaSub::asyndeton`, `LaAdj::gerundive`, `LaNP::adGerund`. Private: Transfer::Ctx
`possRefGender / possRefNumber` (transfer.cpp). Frame (shared with Greek): english.cpp retags (interjection-tagged
imperative before a comma, sentence-final time clause, possessive + past form + noun) and frame_builder.cpp (a verb
tagged amod without a verb form is a participle; the "retag" repair also for interjections). Greek suite unchanged.

### Open points
- "I lost my pen, can I use yours?": a statement and a question joined by a comma become one yes/no question
  ("Perdidīne ... et tuō ūtī possum?", Check).
- "Is this hat yours?": the tagger reads hat as an adjective (Check).
- "Whose shoes are these?" -> cuius + wh order needs a genitive interrogative in LaNP (Check now).
- "all afternoon" hung on the previous noun (parser); "The dog that I saw was big." (relative misparse, Check).

### Review of the pre-loop C20 (main agent, 2026-10-07)
- Accepted: speed fix (signatures computed once; byte-identical output), possessives, asyndeton, tam/tantus/tālis,
  gerundives, mī fīlī. Fresh 12-sentence spot check: OK 3 (2 right), Check 6, Fix 3.
- Faults for C22 (acceptance loop): "Is this your cat or mine?" -> "Estne hoc fēlēs tua aut mea?" rated OK: the
  demonstrative must agree with the predicate noun (haec) and an alternative question takes utrum ... an / -ne ... an;
  "You must not open that door." -> Fix "Nōn patēns illam iānuam": negative obligation is nōlī + infinitive or nōn
  dēbēs; "tomorrow" was read as a noun (prōcrāstinātiō) when the sentence has a future verb: crās; "secret" (noun)
  -> sēcrētum / arcānum; "Give the children something to drink." mixes the cases (puerīs aliquid ad bibendum dā);
  "Whose book is this?" -> Cuius est hic liber?; "without finding" -> sine + gerund or nec ... invēnimus.

## Acceptance loop 1 (C22, 2026-10-07)
Material: the owner's partial acceptance file (82 cues, data/acceptance/, gitignored; copyrighted: no line of it is
quoted here, in the tests or in the tables' examples), the main agent's first-run review (STATUS E5), the owner's own
free Latin version as the reference for register and word choice (his correct choices adopted, his slips not: see the
coordinator's instruction in STATUS C22), tests/regression (own_dialogue EN / ES, oz_sample), tests/samples/sample.en.srt.
tests/heldout/, tests/eval_gold/ and data/work/eval/heldout-* were not opened. Generalisation guard: every rule below
has at least two sentences of our own in `engine/tests/test_rules_en.cpp` (test cases "rules-h: ..."); 20 blind
sentences of the file's registers (children's dialogue, short nonsense song lines, quoted narrative sentences; none
from the film) were written at 13:21 UTC before any change and run once at the end.

### Before / after
| file | start of C22 | end of C22 |
|---|---|---|
| acceptance file, implementer's own expert count (every cue read; any grammar, meaning or vocabulary fault = wrong) | 65 / 82 wrong (main agent, E5) | **24 / 82 wrong** (58 right): song 8 / 31 wrong, quoted narrative 5 / 9, dialogue 11 / 42; wrong among the 16 OK cues 0; automatic errors 25 -> 3 (OK 9 -> 16, Check 48 -> 60, Fix 25 -> 6); output identical over two runs; sheet data/work/eval/alice-c22-after/review_c22_implementer.tsv (gitignored) |
| oz_sample EN (100) | 78 / 100, ok 26, wrong among OK 0 | 78 / 100 (#23 and #13 changed, still unmatched), ok 26 / check 68 / fix 6, wrong among OK 0 |
| own_dialogue EN (114) | 114 / 114, ok 65 | 114 / 114, ok 64 (#112 "You are very kind." is Check now: the gender of "you" is a guess) |
| own_dialogue ES (100) | 100 / 100, ok 81 | 100 / 100, ok 80 (the same sentence in Spanish) |
| sample.en.srt 12, la2x 201, orberg 60 | 12, 201, 60 | unchanged |
| Greek EN->GRC / ES->GRC / GRC->EN, ES / C18 review | 114 / 114, 39 / 40, 40 / 40, 13 / 13 | unchanged |
| blind check (20 own sentences) | first run **11 / 20** acceptable (one sentence replaced before the run, see below) | 14 / 20 after the fixes |

### What changed (C22)
- (1) No cue is emptied or swallowed (sentences.cpp, engine.cpp). Root cause: a closing quote after a space ("was... \"")
  was a chunk of its own, so the sentence stayed open and took the next cue. Closers after spaces now belong to the
  chunk; `endsSentence` skips the space; a stray "|" after a period is a closer. Guard: when a sentence spanning cues
  leaves any cue without Latin words, each cue's part is translated as a sentence of its own (flag `cue-split`, Check).
- (a) Names: a capitalised word of names_la.tsv that the tagger read as a noun / adjective / interjection is the name
  ("Lucy!", "Lucy in the garden"); two-word names are declined as noun + adjective (a nominative adjective reading that
  agrees with the head wins over a genitive homograph: Terra Mīrābilis / in Terrā Mīrābilī, "Potestās Malī" stays);
  titles Mr. / Mrs. / Miss / Ms. before a capitalised word are dropped and a common noun after them is that noun
  ("Mr. Fox, wait!" -> "Vulpēs, manē!"; alone, "Mr. Bear!" -> "Urse!", vocative).
- (b) Words that reached a bracket: adjectives in -ly after "be / feel / look ..." (lonely -> sōlus), a noun-only
  predicate tagged as an adjective (nonsense -> nūgae, also through a noun fallback in the transfer), "say hello / say
  goodbye (to X)" -> salūtāre X / valedīcere X-dat, a dozen / two dozen -> duodecim / vīgintī quattuor (the noun is the
  head), within -> intrā, round -> circum, here / there coordinated in a fragment -> aut hīc aut illīc, a greeting used
  as an adjective (how-do-you-do) -> salūtāns (derived-word, Check), colour + noun compounds (bluebird -> avis
  caerulea, a colour is an adjective, not a genitive), "by / past" as aspect particles.
- (c) Counterfactuals: an "if" clause in the past (or with "could") under a "would" main clause is in the imperfect
  subjunctive (pluperfect for "had had"); a bare second verb shares the first one's auxiliary ("will come and help" ->
  veniet et adiuvābit; "would sit and eat" -> sedērent et ederent); a second verb coordinated with a catenative
  complement is a second complement ("agreed to meet ... and give ..."); "But you would." / "But he will." take the
  previous clause's verb (ellipsis, Check); "There'd be X" / "There would be X" are statements; "I wish (that) ..." ->
  optō ut + subjunctive (imperfect for an unreal wish), never an accusative + infinitive ("Volō semper aestātem
  fuisse" was OK and wrong); song lines "And + bare verb" (also a passive) continue the
  previous song line's clause (subject, tense, mood, modal; Check): "The frogs would live in tiny boats" + "And be fed with honey and cake" -> "Et melle et placentā alerentur".
- (d) Idioms and phrases (phrasebook / states / valency rows): pay attention to -> animum attendere ad + acc (valency
  ad + acc now beats "to + person = dative"); I am late for X (again) -> (iterum) ad X sērō veniō; no time to say
  hello / goodbye (both greetings) -> the teacher's "nōn possum salūtāre neque valēre iubēre"; in a stew -> perturbātus; that's it ->
  ita est; what nonsense -> quae nūgae; once more -> rūrsus (the teacher's choice), from the beginning -> ab initiō;
  after all -> nam (not tamen); at first -> prīmō (one-word adverb rows take the adverb reading); of late -> nūper;
  very much -> valdē; keep + -ing -> semper + verb; go + -ing -> the -ing verb; think nothing of X -> X nihil cūrāre;
  a house of my own -> domus mea (also when "of my own" hangs on the clause); just like / like X -> sīcut + nominative;
  be like X -> similis + dative ("She is like her mother." was "Māter sua est.", OK and wrong); all the other X ->
  cēterī X (the other X stays alius); X, too -> X quoque; important -> genitive of quality magnī / maximī mōmentī;
  "My X and Y!" -> ō + accusative; history (attribute) -> rērum gestārum (the teacher's lēctiō rērum gestārum);
  "with no X" -> sine X ("in it" after it is not translated); "nothing but X" -> nihil nisi X; "X with a Y" (a thing
  describing a noun) -> cum + ablative after the noun; "just / only X" with a predicate noun -> X tantum; "What if
  ...?" -> Quid sī + present subjunctive; "Will / would / could you kindly / please ...?" -> imperative + quaesō (the
  "?" becomes "."); a noun + to-infinitive with its own object -> the genitive of the gerundive (locus domūs
  aedificandae); "Every morning he walked" -> imperfect (a habit); upside down -> pedibus sursum versīs.
- (e) Quoted narrative: appositions between commas are a new structure (`SemNP::apposition`, `LaNP::apposition`,
  realised between commas in the case of the noun: "Duo frātrēs, fīliī molīnāriī, prō rēge pugnāvērunt"); a name read
  as a vocative root before its apposition is the subject; an unfinished sentence ending in "was..." keeps its verb;
  a subjectless past verb continuing a quotation is a statement without its subject, never an imperative ("...
  wanted more bread." was "Quaere pānem.", OK and wrong; now Check); a fronted prepositional phrase read as the root
  noun is the clause's oblique; a "when" line without a question mark is a time clause (ubi, Check).
- (f) Adverb vs adjective after the copula ("That's silly." -> stultum), two English adjectives with the same Latin
  word said once ("very extra special" -> valdē praecipuus), sentence-final ", too" -> quoque (the comma is dropped
  before the parse when the clause has a verb; a verbless fragment takes it as its adverb), generic "one" with a modal
  -> homō; a demonstrative subject agrees with the predicate noun ("This is my cat." -> Haec fēlēs mea est, it was
  hoc and OK), "that / it" + predicate noun is dropped ("That is nonsense." -> Nūgae sunt.).
- (g) The person addressed: a feminine (masculine) name of names_la.tsv in the cue or the previous cue sets the gender
  of "you" and of a child addressed (`Memory::addresseeGender`): "Lucy." + "My dear child, you are tired." -> "Puella
  cāra mea, fessa es." whatever the speaker setting; without a name, a predicate adjective of "you" whose masculine and
  feminine differ is flagged `addressee-gender` (Check), and an order "Don't be silly!" agrees with the person (never
  the neuter).
- (h) Song lines: a fragment of coordinated prepositional phrases keeps each preposition ("Under the moon and over the
  sea" -> "Sub lūnā et super mare", "over" was lost); a phrase after a subject noun in a where-question stays after it
  ("Ubi est pōns super flūmen?").
- Checker (false alarms that made correct Latin Fix): gerunds after a noun / preposition ("tempus salūtandī"), a
  substantive adjective in the dative / ablative in a copula clause ("Omnibus erit placenta"), subject and predicate
  noun of different gender ("omnia lūdibria essent", "hortus meus Terra Mīrābilis esset"; a teacher's tier-1 noun is a
  head even when an adjective homograph exists), a preposition governing the next word is no adjective ("ultrā
  collēs"), a noun governed by its preposition is no modifier ("in mundō meō", mundus "world" vs "clean"). The corruption test stays 200 / 200.
- Tagger repairs (english.cpp): "be (just) like X" (like is the preposition), a noun after my / your / our ("My ears!"),
  a verb-tagged adjective before a noun after "has" ("has thick fur"), a bare word after "and" coordinated with a modal
  or "to" verb ("would sing and dance", "wants to sing and dance"), "dep" verbs with their own "and" are conjuncts.

Rows: names_la.tsv +9 (Wonderland, the Latin name written in an English text, Earth, Edwin, Morcar, Stigand, Mercia,
Northumbria, Canterbury); tiers_la.tsv +34 rows and 10 notes edited (listed in the diff; rūrsus "once more", vīcus
"village", mundus "world" tier 1, vigilia tier 3 so that a watch is hōrologium); phrasebook_en_la.tsv +25 / 1 changed
(after all -> nam); states_en_la.tsv +1 (pay attention); verbprep_en_la.tsv +2 (fight for, declare for -> prō + abl);
preps_en_la.tsv +2 (within, round); valency_la.tsv +2 (attendō ad + acc, valedīcō dat).

API changes (additive): frame.h `SemNP::apposition`; realise_la.h `LaNP::apposition`; transfer.h
`Memory::addresseeGender`, `Memory::songLine / prevSong / prevValid / prevPerson / prevNumber / prevTense / prevMood /
prevModal`; english.h `en::colourWord`. New flags (Check): `cue-split`, `addressee-gender`; doubt `polite-request`
(no Check). Phrasebook patterns may contain punctuation words (", "). Changed test expectation: "She was born in a
small village." -> "In vīcō parvō nāta est." (vīcus row). The frame changes are shared with Greek; the Greek suite is
unchanged.

### Blind check
20 own sentences written at the start (13:21 UTC). One of them was seen by accident while probing the conditional rule
(a girl's "If I were a king ..."), so it was replaced before the run by a new unseen sentence. First run: **11 / 20**
acceptable (lines 1, 2, 6, 9, 13, 15, 16, 17, 18, 19, 20). Wrong: "so early" (māne tam), "We have no time to play now,
Mother is waiting" (relative clause, et), "Don't be silly" (neuter stultum), "If my dog could talk" (poterat), "Hop
and skip and away we go" (no parse), a quoted miller sentence (purpose guess, "by the river"), "Every morning he
walked ... his bread" (perfect, eius), "princess" -> rēgīna and "valdē multum", "I'm late for school again" (prō
lūdō). Fixed with own sentences ("rules-h: fixes after the blind check"): order to a person agrees with the person,
unreal condition with could, habitual "every" in the past, late for X again, very much. After: **14 / 20**.

### Open points
- Cues still wrong by my count: lines the parser cannot build (a long quoted sentence split over two cues, a
  wh-question with a stranded "for", a "think nothing of" clause interrupted by an ellipsis), the nonsense
  philosophy lines (free relatives "what it is" with negations), a relative clause that continues the previous song line, a quoted
  onomatopoeia dropped, "date" (appointment) has no Latin noun in the library, "babbling" as a noun, "understand" is
  simplified to sciō by periphrasis_la.tsv, cue splits that move a coordinated verb or a phrase into the neighbour cue.
- "Hop and skip and away we go!" and other nonsense song lines without a subject still fail the parser.

### Review of acceptance loop 1 (main agent, 2026-10-07)
- Accepted. Re-review of the 82 cues: 22 wrong (first run 65), none of the 16 OK cues wrong. By register: dialogue
  8/42, song 9/31, quoted narrative 5/9. The empty-cue bug is gone and nothing is bracketed except one invented word.
- For loop 2 (C24): per-cue speaker gender (a male character in a feminine-speaker file got feminine predicate
  forms); the long quoted narrative sentences still do not parse (fall back is honest but useless: a cue-local
  clause split before the no-parse fallback); nonsense free relatives "what it is, it wouldn't be" (quod est, nōn
  esset); sequence after optō; "babbling" as a participle (murmurāns); "falling down stairs" -> dē scālīs cadere;
  "upside down" -> inversus / capite deorsum; "I wonder where" -> mīror ubi sit.

## Latinity (C23, 2026-10-07)
Owner decision D18: Medieval and ecclesiastical Latin are accepted behind the setting `latinity`, "wide" by default.
- Settings (engine/core): default `latinity` "wide"; validation "wide" | "classical" (a bad patch is `bad_params`, a bad
  value on disk is repaired to "wide" with a warning). CLI `engineOptions()` sets `rules::Options::latinity`
  (enum `Latinity {Wide, Classical}`) for translate.start and orbergise.start; anything but "classical" is "wide".
- Transfer (`Transfer::select`): the reverse index already subtracts 30 of 255 at build time for a sense tagged rare /
  archaic / poetic / Medieval (SENS bit 6, "medieval": Medieval, Late, ecclesiastical, Vulgar Latin) / New Latin
  (bit 7) (gloss.py `PENALTY_BITS`, applied once). With "wide" a sense whose only register tags are bit 6 / bit 7 gets
  the 30 back (integer score units, so ties stay exact ties broken by lemma id; the compensated base also feeds the
  weak-sense and tier-preference tests); `why` says "late Latin accepted". With "classical" nothing changes in the
  scores. A penalty that came from a tagged translation-table row (gloss.py `PEN_ROW_TAGS`) is not visible to the
  engine and is not given back. Every choice records the chosen sense's register (`Choice::registerTag`
  "medieval" | "new-latin"; taught lemmas use their first sense; cleared by a periphrasis).
- Confidence: with "classical" a choice of kind sense that still lands on a tagged sense makes the cue Check, flag
  `late-latin`, and a `sense` reason "... only a Medieval / Late Latin word was found, and classical Latin was asked
  for: check it". With "wide" the register is never a Check reason. A6 is about tiers only and was not touched (a
  Medieval word of tier 3 is still Check at fidelity 2/3 for its tier, not for its register).
- Tokens: `TokenView::registerTag` -> view `TokenView.register` on the token of the chosen lemma; the sense reason
  text adds "(Medieval / Late Latin sense)" or "(New Latin sense)". words.list prints `register` per word.
- Phrasebook: register cell `eccl` or `eccl+<register>` (`eccl+polite`, `eccl+greet`, `eccl+excl`) sets
  `PhraseEntry::eccl` and keeps the functional register for the frame builder's placement rules. The frame
  builder's pre-pass skips eccl rows with "classical" (`FrameBuilder::analyse(..., classical)`,
  `Phrasebook::match(..., classical)`). Two rows of the same pattern, one eccl, are twins: the earlier one is chosen
  ("wide"), the other is offered as an alternative ("classical rendering of ..." / "ecclesiastical rendering of ..."),
  never an eccl twin with "classical". The phrasebook reason data carries `"register":"eccl"` for an eccl row.
  Rows: I'm sorry / lo siento -> habeās mē excūsātum/excūsātam (eccl, before ignōsce mihi); goodbye / adiós -> deus tē
  servet (eccl, after valē); thank God / gracias a Dios -> deō grātiās (eccl) with the classical twin deō grātiās agō;
  please / por favor stays quaesō.
- Own sentences (engine/tests/test_rules_en.cpp "rules-i"): "I'm sorry." -> Habeās mē excūsātam. (f) / excūsātum. (m),
  alternative Ignōsce mihi. ("wide"), Ignōsce mihi. ("classical"); "Goodbye." -> Valē. in both, Deus tē servet. only as
  the "wide" alternative; "Thank God!" -> Deō grātiās! / Deō grātiās agō!; Spanish mirrors; "The alchemist is here." /
  "Where is the alchemist?" (alchēmista, Medieval only; fidelity 1) OK in "wide", Check + late-latin in "classical";
  "knight" -> mīles (its Medieval sense) in "wide", eques in "classical" (fidelity 1); two fresh engines per mode give
  byte-identical output.
- Regression files with the default "wide" (fidelity 2, speaker f): EN own_dialogue 114 / 114 (outputs and confidences
  identical), ES 100 / 100 identical, oz_sample 78 / 100 with two lines changed, Greek EN->GRC 114 / 114, ES->GRC
  39 / 40, la2x and orberg reports identical. Changed oz lines (neither matches its gold before or after; no gold
  changed): 67 "You have called us for the third and last time." Tertium et ultimum nōs vocāvistī. ok -> check (the
  candidate clāmō of a Medieval sense of "call" comes within 0.15 of vocō: margin Check); 93 "Hereafter you will be a
  great man, for I have given you ..." Posthāc vir magnus et dēlātor eris. (fix) -> ... et auctor eris. (check) (the
  parse reads "given" as a noun; auctor in a Late sense now passes the tier-preference floor). With "classical" the
  regression outputs equal the pre-C23 ones; only thēa (New Latin) cues become Check: EN 36, 37, ES 37.
- Not done / for the main agent: Orbergise without an original (la -> la rewrites by la2x analysis) does not look at
  the register; Greek is untouched (Byzantine "medieval" tags are not compensated). docs/DESIGN.md §9.1 says "SENS tag
  bit5" for the register: in the SENS tags it is bit 6 (medieval) / bit 7 (New Latin); bit 5 is the ANAL flag
  LateLatin of forms (and SENS "poetic").

## Acceptance loop 2 (C24, 2026-10-07)
Material: the owner's partial acceptance file (82 cues, data/acceptance/, gitignored, copyrighted: no line of it, of
the owner's Latin or of a close paraphrase appears here, in the tests, in the tables or in a commit; cues are named by
number and by the fault category of the main agent's sheet data/work/eval/alice-c22-main/review_main_agent.tsv), the
loop-1 output and review, tests/regression, tests/samples. tests/heldout/, tests/eval_gold/ and data/work/eval/heldout-*
were not opened. Every rule below has two or more sentences of our own in engine/tests/test_rules_en.cpp (test cases
"rules-j: ..."), written before the rule was run on the file. Blind check: 20 own sentences of the registers
(children's dialogue, nonsense song lines, quoted narrative) written at 16:31 UTC before any change
(data/work/eval/c24-blind/, gitignored), run once at 17:42 UTC.

### Before / after
| file | start of C24 | end of C24 |
|---|---|---|
| acceptance file, implementer's own count (every cue read; any grammar, meaning or vocabulary fault = wrong) | 22 / 82 wrong (main agent, E7) | **3 / 82 wrong** (cues 18 and 19: quoted fragment whose subject lies outside the file, singular verb and sense; cue 79: the "upside down" phrase stands after the main verb instead of inside the relative clause): song 9 -> 0 / 31, quoted narrative 5 -> 2 / 9, dialogue 8 -> 1 / 42; OK 16 -> 20, Check 62, Fix 4 -> 0, automatic errors 3 -> 0; wrong among OK 0 by my count |
| oz_sample EN (100) | 78 / 100, ok 25, wrong among OK 0 | 78 / 100, ok 25, wrong among OK 0 (#30 and #80 still unmatched, Fix -> Check) |
| own_dialogue EN (114) / ES (100) | 114 / 114, 100 / 100 | identical reports |
| sample.en.srt 12, la2x 201, orberg 60 | 12, 201, 60 | unchanged (reports identical) |
| Greek EN->GRC / ES->GRC / GRC->EN, ES / C18 review | 114 / 114, 39 / 40, 40 / 40, 13 / 13 | identical reports |
| blind check (20 own sentences) | first run **7 / 20** acceptable | 11 / 20 after the fixes |

### What changed (C24)
- (a) The speaker of a reply. Per cue, the engine keeps the gender of the person addressed by name or title and
  whether the cue's last sentence asks for an answer (a question, an order, or the address alone). A cue that answers
  such a cue is spoken by that person, and so is a dash turn after such a turn in the same cue: "Grandfather, are you
  tired?" + "Yes, I am very tired." -> "Ita, valdē fessus sum." with a feminine project speaker. When that gender
  differs from the project setting and the Latin depends on it, the cue is Check (flags `speaker-gender` and
  `speaker-reply`, a reason, and the setting's rendering as the first alternative). Gender of an address: the
  project glossary's gender, names_la.tsv, a title (Mr., Sir / Mrs., Miss, Madam), or a short English list of nouns
  that say the sex (father, mother, uncle, girl, king ...); "child", "friend" and animals give none. A goodbye does
  not ask for an answer. The same gender sets "you" when no name did ("Mother, are you tired?" -> "esne fessa?").
  Kinship words before a comma are nouns of address, not orders ("Grandfather, ..." was "Et, ... [grandfather]").
  DESIGN §7 / §8 have no per-cue speaker field, so a user tag per cue is not possible: the heuristics use the Names
  tab gender (Context.glossary) and names_la.tsv; a per-cue speaker field in cues.jsonl would be needed for more.
  Phrasebook rows "valdē perturbātus/perturbāta sum": two sides of the same length alternate as wholes only when
  they are parallel word by word (same first letters: "mī amīce/mea amīca"); else one word alternates (the old rule
  lost "sum" or "valdē").
- (b) Long quoted narrative: a name root with its apposition and a verb after the second comma hung on it as dep / acl
  is rerooted (the verb is the root, the name its subject: "Even Paul, the Bishop of Rome, promised ...");
  the cue split keeps a word without a source word of its own (a name, a pronoun, et, a preposition) with its
  neighbour (a preposition with its noun, a pronoun with its verb). Before the word-by-word fallback (could-not-parse, Fix) the sentence
  is cut at commas and and / but / or / because / when / while / so / then and translated clause by clause (flag
  `clause-split`, Check) when at least one clause gives a Latin verb.
- (c) Free relatives with "what": a small grammar gives the tree of sentences made only of "SUBJ VG what SUBJ VG",
  "what SUBJ VG, SUBJ VG" and "what ... be what ..." clauses joined by because / since / and / but (flag
  `free-relative`, Check): "what" heads its relative clause and is the predicate of "be" or the object; the Latin
  antecedent id is understood (`LaNP::elideHead`): "The sea would be what the sky is." -> "Mare esset quod caelum est.",
  "What you have, you keep." -> "Quod habēs tenēs." A verb group of auxiliaries only takes the relative clause's verb
  ("What the cat won't eat, the dog will."); "it" in the relative clause takes the plural of
  a plural main subject ("Everything is what it seems ..." -> omnia sunt quod videntur). The free relative follows the verb unless the source put it first;
  the checker accepts a neuter quod after a verb. After know / wonder / ask / tell / see ... "what" stays an indirect
  question. "seem" is the passive of videō (vidētur; it was "videt"), and predicate agreement counts videor / fīō as
  copulas.
- (d) Sequence of tenses after a verb of wishing: present optō -> present subjunctive ("Optō ut volāre possim"),
  a past one -> imperfect. Changed expectations (C22 tests): "I wish it was always summer." -> Optō ut semper aestās
  sit; "She wishes that he would come." -> Optat ut veniat; "I wish I could fly." -> Optō ut volāre possim.
- (e) Word choices: roll by -> praetereō, roll away -> āvolō (phrasal rows); leave + object -> relinquō (phrasal row
  "relinquō|-": without an object the verb's own word; a "-" alternative is the verb's own translation); an -ing
  word before its noun read as a noun compound is the verb's present participle when English has no such noun or
  Latin only a rare one (aqua murmurāns, leō rudēns; "babble" -> murmurō); understand -> intellegō (periphrasis row
  removed: sciō changed the meaning); "down" before the noun of a verb of motion is dē + abl ("The children ran down the hill." -> Puerī dē colle cucurrērunt);
  upside down -> capite deorsum; an adverbial phrase that is the predicate of "be" makes "be" the clause's verb and
  stands before it ("He is in trouble." -> "In perīculō est."; it was "Es in perīculō."); afternoon rows (post
  merīdiem, diē + adjective + post merīdiem: an ADJ slot may carry case and gender "{1:abl.m}"); a "tail" phrase may
  open the sentence; "I don't know where." -> "Nesciō ubi sit."; a hesitation
  inside a verb group ("must... be going") and a word repeated after an ellipsis ("in... in the lake") are read once;
  "What could a fox possibly want?": the noun after the auxiliaries is the subject, "what" the object or the object
  of the stranded preposition ("What are you afraid of?" -> "Quid timēs?"; it was "Quid timidum?", OK); "could" with
  "possibly" in a question is present (potest); be late for X -> sērō venīre ad X; "After this he ..." -> post hoc.
- (f) Relative clauses: a song line that opens with that / which / who after a line ending in a noun is that noun's
  relative clause (analysed with the noun in front, the noun's Latin word taken out again; when the line ends in a
  prepositional phrase the noun before it is the antecedent; flag `song-relative`); "where" + subject + verb after a
  noun is a relative clause of that noun (in quō / in quā), and a place noun without a preposition after a verb of
  motion is the goal (in + acc) unless a path word (down, along, across ...) stands before it; a "can" relative clause inside a "can" clause is in the subjunctive ("Librum quem
  legere possem invenīre poteram").
- (g) A sound word in quotes ("woof", "moo": one or two English interjections or unknown words, not yes / no /
  hello ...) is kept as written: the parser reads "it" in its place (offsets kept) and the Latin pronoun is replaced
  by the quoted word ("Canis \"woof\" dīcit."); a clause complement that opens with its own and / but / or is a
  coordinated clause ("Cats say it and dogs say it." -> "... et canēs id dīcunt").
- (h) An invented word of a preposition (under, over, beyond, behind, inside, outside) and a known noun is read as
  that phrase, flag `derived-word` (Check): "underbridge" -> sub ponte, "overcloud" -> super nūbem (as an adverb an
  oblique of its own).
- After the blind check: put on (clothes) -> induō (verbprep row, only when the verb has no other object); ", or" after
  a command or a "must" -> "; aliter"; a second "if" clause joined by "and" is unreal too; "like" chosen as placeō
  turns the roles round (the thing liked is the subject, the one who likes it the dative: "I think she likes him." ->
  "Putō eum eī placēre"; it was "eam eī", the reverse meaning); a coordinated verb without its own subject takes the
  reflexive possessive (mātrem suam).

Rows: tiers_la.tsv +6 (murmurō, scālae, āvolō, deorsum, merīdiēs, occupātus) and 1 note (pars "part, side, share");
phrasal_en_la.tsv +3 (roll by, roll away, leave); phrasebook_en_la.tsv +5 (afternoon x4, at dawn) and 1 changed
(upside down -> capite deorsum); preps_en_la.tsv +1 (down -> dē + abl); verbprep_en_la.tsv +1 (put on -> induō);
periphrasis_la.tsv -1 (intellegō -> sciō).

API changes (additive): realise_la.h `LaNP::elideHead`. New Check flags: `speaker-reply` (with `speaker-gender`),
`free-relative`, `clause-split`, `song-relative`. Phrasebook ADJ slot "{n:case.gender}". No CLI or core change.

### Blind check
20 own sentences written at 16:31 UTC (before any change) and run once at 17:42 UTC: **7 / 20** acceptable (a
kinship address, a why-question, an indirect "what" question, a comparison, a made-of sentence, a song line of
genitives, a plain narrative line). Wrong: a coordinated infinitive under licet and the noun kitten, "he likes me"
with the roles reversed, "tell X that ..." read as a relative clause, ", or" as aut, "too small to", "tired of
waiting", a second unreal condition in the indicative, fronted PPs with a contact relative, "Round and round ...", a
long sentence with an appositive relative clause (quis), "called them ... and asked", a "when" clause with an
appositive pair, "put on his coat" and "his mother" (eius). Fixed with own sentences ("rules-j: fixes after the blind
check"): the like / placeō roles, ", or" after must -> aliter, the second unreal condition, put on -> induō, the
reflexive possessive of a coordinated verb. After: **11 / 20**.

### Open points
- Cues 18-19 (a quoted fragment whose subject is outside the file: the number of the verb is a guess) and cue 79 (a
  closing phrase that belongs to the relative clause is placed after the main verb).
- Long narrative sentences with appositive relative clauses and "when" clauses with apposed adjectives still parse
  badly (blind lines 17-19); the clause split avoids the word-by-word Fix but the joins are rough.
- "too ADJ to VP" (quam ut), "tell X that ..." (accusative and infinitive), "tired of + -ing", a coordinated infinitive
  under licet are not handled.
