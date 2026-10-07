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

## Real material (C27, 2026-10-07)
E11 ran Orbergise on the owner's own Latin subtitle file (83 cues; copyrighted, never committed or quoted: cues are
referred to by number only, every test sentence here is our own) and found meaning-changing swaps rated OK with the
meaning check at 100 %, nonsense structure rewrites, emoji clusters split, macrons added to a macron-less text and two
cues replaced by a retranslation of the English. What changed:

1. **Same-sense swaps only** (`tables.cpp` swapFor / sameSense). Candidates in this order: `syn` rows of
   simplify_la.tsv (new kind: confirmed same-sense pairs, the only swaps that may stay OK), periphrasis_la.tsv rows
   (Check), then teacher glosses and the lexicon's senses, kept only when they are the same sense: an English gloss
   phrase shared (phrases, not single keywords: "leave" is not "leave off"; the candidate's first meaning among the
   word's or the other way round), real (non-pivot) Spanish glosses sharing a phrase and covering every Spanish sense
   group of the word (tempestās "tiempo; tempestad" is not tempus), else both English first meanings shared; sense
   classes (new kind `class`: motion / change / cease / return; a verb of motion never becomes one of the others);
   transitivity and valency; a noun of a person keeps its sex (magistra is not doctor). No such word: the word stays,
   the cue is Check (flag `tier-kept`) and a note says why ("kept: no first-year word with the same sense (tier 2)").
   A written form that is also a form of a core word in any reading is never swapped (meō is also meus), nor a word
   whose reading competes with another lemma's ("vadis": vādō / vadum).
2. **Fixed phrases** (new kind `fixed`, `rewrite.cpp` protectFixed): rēs gestae, grātiās agere, valēre iubēre,
   ad + gerund, Deō grātiās, habeās mē excūsātum, pāx tēcum, miserēre nōbīs, magnī mōmentī, orbis terrārum, quid
   agis, ōrō tē; elements by lemma (participles by their verb), gap and order per row. Their words are never split,
   rewritten or swapped; a word above the ceiling inside one is kept with the note "fixed phrase ...".
3. **Never nonsense** (`orberg.cpp` sound()): every rewritten sentence is re-analysed; a rewrite that adds an
   A1/A3/A4 fault the input did not have, makes a swapped word read as another word or adds an unknown word is
   discarded: vocabulary-only is tried, then structure-only, then the sentence as written (flags `rewrite-partial` /
   `rewrite-discarded`, Check, note with the reason). Rule fixes for the two structure rewrites E11 saw: the ablative
   absolute needs a noun the analyser reads as an ablative (a nominative next to est is the subject), cum + subjunctive
   needs no other subordinator between cum and the verb (cum + ablative + ut clause is the preposition).
4. **Meaning check** (`orberg.cpp` meaningOf): the output is re-analysed by la2x and the content lemmas a reader reads
   (best readings, not every reading) are compared with the input's; a lemma is kept when it is there, or when it
   was replaced by the same written word read otherwise, a rule's regeneration, a multi-word teacher row, or a word
   whose glosses share its sense (la2x curated gloss + the lexicon's first meanings, or real Spanish glosses, and no
   class conflict). Losses are listed ("moveō -> mūtō") and make the cue Check (`meaning-lost`; below 0.6 also
   `meaning-low`). With an original: a word of the original the input covers (la2x round trip, simple English base
   forms) and the output does not is listed as `"word" (original)`. Tested with deliberately wrong syn rows.
5. **Emoji clusters** (`la2x/analyse.cpp`): the tokeniser keeps a grapheme cluster as one symbol token (variation
   selectors, skin tones, ZWJ sequences, flags, keycaps); the analyser disambiguates without them (an emoji between a
   noun and its adjective no longer cuts the agreement: "hortō🌳 meō" is meus) and merges them back
   (`Token::symbol`); Orbergise analyses without them (`Analyser::analyseWords`) and copies the source's text between
   tokens, so emoji and spacing come back byte for byte. The fix belongs to la2x (the la-la path's tokeniser), not
   to engine/core or engine/subs: no other pair changed (EN 114/114, ES, Greek, la2x 201/201 unchanged).
6. **Macrons follow the source**: cues() reads the document's convention from the batch (a length mark anywhere: with
   macrons; none: without, new words get none, `OrbergOptions::sourceMacrons`); a single sentence without that
   information decides from words that visibly omit a length mark (capitals and anceps endings no longer mislead).
   An unchanged cue keeps its own lines byte for byte (no relayout); the first word keeps the source's case.
7. **The original is evidence, never the output**: EngineContext::fromOriginal is no longer called. The original's
   words rank same-sense candidates (English originals), enter the meaning check, and name the person of a gerund of
   obligation without an agent ("Eundum est." + "We must go." -> "Īre dēbēmus."; one-sentence cues). Flags
   `original-evidence` and `orberg-latin` / `orberg-kept`; `orberg-original` is gone. simplify_la.tsv
   `rule original use=evidence`. DESIGN §10.7 ("build the SemFrame from the original and re-realise") is superseded
   for the main agent to update.
8. **Confidence**: a swap is at most Check (flag `synonym`) unless every swap of the cue is a `syn` row.

### Measured on the owner's file (counts only; engine before = dba5799)
| run | cues changed (text) | OK / Check / Fix | distinct swaps (right / marginal / wrong) |
|---|---|---|---|
| tier 1 before | 21 (+15 relaid out only) | 33 / 43 / 7 | 17 (E11: 6 / 3 / 8) |
| tier 1 after | 13 (0 relaid out) | 38 / 38 / 7 | 9 (my count: 9 / 0 / 0) |
| tier 1 + English original before | 24 (2 replaced by a retranslation) | 36 / 41 / 6 | |
| tier 1 + English original after | 13, identical to the run without | 38 / 38 / 7 | same 9 |
| tier 2 before / after | 3 / 0 | 57 / 19 / 7 -> 56 / 20 / 7 | magistra -> doctor gone |
After: 0 structure rewrites fired (the two before were nonsense), 0 emoji clusters changed, 0 macrons added, 0
meaning losses; 41 cues are Check with `tier-kept` (words above the ceiling without a same-sense core word: the
teacher decides). The 7 Fix cues are the source's own (6 unchanged cues: interjections the lexicon does not know,
checker faults on the owner's Latin; one cue changed only by magnifica -> magna).

### Blind check (C27)
20 own sentences in the owner's register (`tests/fixtures/orberg/blind_c27.tsv`), run once at tier 1 through the
engine BEFORE any change and judged only at the end: **first run 11/20 acceptable**; the 9 others had emoji split
(5 sentences), wrong swaps (rēs portātae, mūtantur, dēsinunt, redeunt), a narrowed noun (fōns -> aqua) or marginal
swaps (colligit -> legit, aspicit -> videt), several of them more than one fault.
After the C27 fixes the set showed one new wrong swap of my own making (tempestās -> tempus by the dictionary's first
sense: fixed by the Spanish sense-group rule) and aspicit -> videt (now spectat by a syn row); then 20/20.

### Fixtures changed
`sentences_more.tsv` #71 expects spectāvit (looked at) instead of vīdit; `with_original.tsv` under the evidence
policy: 8 rows that C14 solved through the original keep the Latin (cōnsīdit, parat, alloquitur stay, Check) or take
the original's person (Īre dēbēmus; without macrons where the cue has none). Own set 60/60, second batch 20/20, blind
C27 20/20, with original 22/22.

### Gaps
* Many tier-2 words of real material have no same-sense core word (hostis, castra, cuniculus, convīvium ...): they
  stay, Check. More `syn` rows are the teacher's lever (each is a promise that the pair is the same sense).
* "Quo vadis?" stays: vadis competes with vadum. Polysemous words are kept unless a syn row says otherwise.
* The original only ranks candidates and names a person; it does not choose a sense against the dictionary.
