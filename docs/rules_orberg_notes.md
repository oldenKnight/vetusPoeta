# Orbergise (C14, `engine/rules/src/orberg`) — notes

Files: `engine/rules/include/vp/orberg.h` (options, result, `orbergise()`, `cues()`, `Resources`, `EngineContext`),
`src/orberg/{internal.h, tables.cpp, rewrite.cpp, orberg.cpp}`, tests `engine/tests/test_rules_orberg.cpp`, fixtures
`tests/fixtures/orberg/{sentences.tsv (60), sentences_more.tsv (20), with_original.tsv (22)}`, tables
`data/curated/simplify_la.tsv` (new) and 19 rows appended to `periphrasis_la.tsv`. Engine wiring: one block
"C14 orberg ... BEGIN/END" at the end of RulesEngine plus two one-line hooks marked "C14 orberg hook" (include; pair
la-la dispatched in translate()).

## Pipeline (one cue = one or more sentences, la2x::splitSentences)
1. **Analyse** with the la2x Analyser (constraint propagation). Rules look at every reading of a word, not only the
   best one (the analyser prefers nouns to participles: `scrīptūra`, `ingressus`, `vīsīs`).
2. **Rewrite in place** (`rewrite.cpp`): one output slot per input token; rules replace, delete or insert words and
   regenerate forms with `morph::generate`; untouched words keep their written form. A sentence without a change is
   returned byte for byte. Rule order: pairs, ablative absolute, gerundive, future participle, supine, cum +
   subjunctive, historic infinitive, relative chain, long-sentence split, acc + inf mark, then vocabulary.
3. **Vocabulary**: a word whose lemma is above the ceiling (participles count with their verb's tier) is swapped:
   `periphrasis_la.tsv` first (the word of the row with the lemma's part of speech is inflected, the others are fixed:
   festīnat -> celeriter it), then the teacher glosses of `tiers_la.tsv`, then the lexicon's sense keywords (REVX,
   sense 0/1 of the candidate, score >= 100, same part of speech, valency compatible; verbs: the candidate's sense names
   the word's first meaning or its first meaning is among the word's first two; nouns/adjectives: the candidate's first
   meaning is the word's and two keywords overlap). Deponent <-> active handled (ingressus est -> intrāvit; ēgrediuntur
   -> exeunt); a noun of another gender regenerates its agreeing modifiers (nāvigium magnum -> nāvem magnam). A lexicon
   synonym (not a curated row) sets flag `synonym` (Check): the teacher confirms it. `keep` rows block bad keywords.
4. **Checks**: A1-A4 by the Latin checker on the output; A6 here (participles with their verb's tier, names exempt);
   A7 = meaning; A8 (cps, lines) for cues. Confidence: Fix on A1/A3/A4 or unknown words; Check on A6, meaning < 60 %,
   `synonym`, `structure-kept`, `agent-guess`, `frame-fallback`, A8; else OK.
5. **Meaning check**: content lemmas (nouns, verbs but sum, adjectives, adverbs, participles -> verbs, names, numerals)
   of the input against every reading of the re-analysed output; swaps, pair rows and the readings a rule chose count as
   the word they replace (followed three steps). `missing` lists the heads not accounted for; nothing is dropped
   silently (the agent pronoun of a gerundive and the copula are not content words).
6. **With the original** (CueInput.originalText): see "Decisions" 1.

## What is rewritten (simplify_la.tsv, each rule switchable, parameters in the row)
| rule | input | output |
|---|---|---|
| ablabs.perf | Epistulā lēctā, puer domum cucurrit. | Postquam epistula lēcta est, puer domum cucurrit. |
| ablabs.perf (deponent, swap) | Patre profectō, fīlius ... mānsit. | Postquam pater abiit, fīlius ... mānsit. |
| ablabs.perf (agent phrase) | Epistulā ā patre scrīptā, ... | Postquam epistula ā patre scrīpta est, ... |
| ablabs.pres | Sōle oriente, avēs cantābant. | Dum sōl surgit, avēs cantābant. |
| gerundive (agent) | Mihi epistula scrībenda est. / Puerō liber legendus est. | Epistulam scrībere dēbeō. / Puer librum legere dēbet. |
| gerundive (no agent) | Porta claudenda est. | Porta claudī dēbet. |
| gerund + dative | Nōbīs domum eundum est. | Domum īre dēbēmus. |
| supine | Servī aquam petītum ad flūmen iērunt. | Servī ad flūmen iērunt ut aquam peterent. |
| futpart | Puer ad urbem itūrus est. / Puella epistulam scrīptūra erat. | Puer ad urbem ībit. / Puella epistulam scrībere volēbat. |
| histinf | Puerī clāmāre, puellae rīdēre. | Puerī clāmābant, puellae rīdēbant. |
| cumsubj | Cum puer domum vēnisset, ... / Cum servus ... labōrāret, ... | Postquam puer domum vēnit, ... / Dum servus ... labōrat, ... |
| pairs | Haud longa / Nōn nūllī puerī / Nōn numquam / Nōn nihil / nōn nescit | Nōn longa / Aliquī puerī / Saepe / Aliquid / scit |
| relchain | ... quem pater ēmit quī in urbe labōrat. | ... quem pater ēmit. Pater in urbe labōrat. |
| split (> 12 words) | ... cucurrit et ibi ... lūdēbat et sorōrem ... audīvit. | ... cucurrit. Ibi ... lūdēbat. Sorōrem ... audīvit. |
| accinf | Patrem existimō bonum esse. | Patrem putō bonum esse. (structure kept, note) |

## Measured (data/work of 2026-10-06, ceiling 1 unless the row says 2)
| set | result |
|---|---|
| sentences.tsv, 60 own sentences | 60/60 normalised (60 byte-exact); 57 rewritten, 3 unchanged; 0 A1/A3/A4 faults, 0 tier faults, meaning 100 % on all |
| sentences_more.tsv, 20 written afterwards | 20/20 on the first run, unchanged since; 0 faults |
| with_original.tsv, 20 EN + 2 ES cues | 19/22 (EN 17/20); path (latin / original / kept) as expected 22/22 |
| same, policy prefer=original | 12/22 exact; real errors in 5 (ut aquam petat; "some" dropped; agit; servīs loquitur; tranquillus) |
| 1,000 cues through the engine | RssAnon about 2,650 kB after 10 cues and the same after 1,000; two independent contexts byte-identical |
Honesty: the first 23 sentences of the 60 were run through the analyser before the expectations were written, and four
fixes followed the first run of the 60 (merged genders, neuter object case, `vēnit` read as vēneō, a missing comma);
the second batch was written blind and passed unchanged. EN -> LA regression stays 114/114, la2x and Greek tests green.

## What is left as it is (gaps)
* Conjunct participles (Puer currēns cecidit), noun + noun ablative absolute (Caesare duce), supine in -ū (mīrābile
  dictū), future infinitive in acc + inf (sē ventūrum esse), gerundive/gerund without an agent of a deponent or an
  impersonal (Eundum est): kept; the last one sets `structure-kept` (Check) and is where the original helps.
* Words above the ceiling without a core word of the same sense stay (hostis, dux, vincō, habitō, lūdus, parō,
  pulcherrimus): A6 fails, Check. With an original, its translation is tried (often it uses the same tier 2 word).
* Ablative absolute is rendered passive ("postquam epistula lēcta est") because the agent is not in the text; the row
  parameter voice=active gives the active with the main clause's subject (flag agent-guess, Check).
* cum + imperfect subjunctive is read as "while" (dum + present); causal/concessive cum is not distinguished.
* The relative-chain and split rules are deliberately narrow (last relative clause at the sentence end; et/sed/nam/
  atque between two finite clauses).

## Decisions taken here (for review)
1. **Original policy** (`rule original prefer=rewrite`): the rewrite of the Latin is used when it is clean (no Fix,
   ceiling met, meaning >= 60 %, no construction left); else the original's translation (EN/ES -> LA, fidelity 3,
   the input's own core lemmas forced where the transfer had them as candidates) when that is clean. Reason: measured
   above, original-first produced 5 wrong cues of 22 and never a better one. `prefer=original` restores DESIGN §10.7.
2. Meaning reference: the input Latin for a rewrite; the original's transferred lemmas when the original was used.
3. New words get macrons unless the input visibly lacks them (a word whose reading has a macron the text omits).
4. A6 counts a participle with its verb's tier: the library gives every participle lemma tier 3 (lēcta, inventae).

## Findings for other modules
* Checker (C1): A6 reads participle lemmas (tier 3) instead of their verbs, so "epistula lēcta est" fails A6 there.
* la2x analyser (C11): `vēnit` (macron) read as vēneō "is sold" before veniō perfect; neuter nom/acc after another
  nominative read as a second nominative (Nauta nāvigium vīdit); perfect participles of deponents read as nouns
  (ingressus est); relative clauses are not segmented as clauses.
* EN -> LA (C2/C13): purpose clause "to get water" -> "ut aquam petat" (number and sequence of tenses), "make dinner"
  -> agit, "speak to" -> loquor + dative, "be quiet" -> tranquillus esse.
* My periphrasis rows also apply to EN/ES -> LA at fidelity >= 2 (that is the table's contract); rows that would
  narrow a meaning there (iuvenis -> vir, subitō -> statim) were left out.

## Questions
1. Keep the rewrite-first original policy, or follow §10.7 literally (prefer=original)?
2. Passive (default) or active-with-main-subject for the ablative absolute?
3. `futpart` past (scrīptūra erat -> scrībere volēbat) shifts "about to" towards "wanted to": acceptable, or keep it?
4. Should A6 in the checker adopt the participle -> verb tier rule?


## Decisions by the main agent (2026-10-06)
1. Rewrite-first stays (the Latin's own rewrite when clean, the original's translation as fallback): the measured
   12/22 vs 19/22 settles it. `simplify_la.tsv` keeps `prefer=rewrite`.
2. Ablative absolute -> passive temporal clause by default (no invented agent); the active switch stays optional.
3. Gerundive without agent -> dēbeō + passive infinitive. Accepted.
4. Dictionary-driven swaps marked Check for the teacher. Accepted.
