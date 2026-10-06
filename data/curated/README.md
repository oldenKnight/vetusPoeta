# Curated data (hand-written, part of the proprietary program; tab-separated, UTF-8, `#` comments allowed)

All files are written by the project itself (main agent and teacher); nothing here is copied from a textbook or a
dictionary. Latin headwords carry macrons; the `key` column is `latin_key(head)` (DESIGN §4). Lines starting with `#`
and blank lines are ignored by every loader. Loaders live in tools/build_library (build time) and engine/rules
(runtime copies are embedded into the lexicon or shipped as `data/*.tsv` next to the engine; decided per file below).

| File | Columns | Used by |
|---|---|---|
| `tiers_la.tsv` | key, head, pos, tier (1/2), source (derived/teacher), note | build: LEMM.tier; engine A6, transfer (tier wins over the lexicon's; note = English glosses) |
| `valency_la.tsv` | key, frame, example, note | engine transfer/realise (A4) |
| `preps_en_la.tsv` | english, context, latin, case, note | engine transfer |
| `contractions_en.tsv` | form, expansion | engine nlp |
| `phrasebook_en_la.tsv` | pattern, latin, tier, register, note | engine frame pre-pass |
| `names_la.tsv` | english, latin_nom, latin_gen, gender, declension, policy, note | engine names seed |
| `emoji_la.tsv` | key, head, emoji, note | build: LEMM.emoji; engine display |
| `nonverbal_en_la.tsv` | english, latin | engine cue assembly |
| `periphrasis_la.tsv` | key, tier, periphrasis, note | engine transfer (fidelity 2/3) |
| `gloss_es_la.tsv` | key, head, gloss_es | build: LEMM.gloss_es, SENS |
| `macron_overrides.tsv` | key, stem_from, stem_to, note | engine realise (Macrons) and phrasebook words |
| `phrasal_en_la.tsv` | verb, particle, latin, frame, note | engine transfer (phrasal verbs) |
| `verbprep_en_la.tsv` | verb, prep, latin, frame, note | engine transfer (verb + preposition senses) |
| `states_en_la.tsv` | source, latin, kind, note | engine transfer ("be" + state adjective with a person subject) |

Frames in `valency_la.tsv`: `acc` direct object accusative; `dat` dative object; `abl` ablative object; `gen` genitive
object; `dat+acc` (dare); `acc+inf`; `inf` (possum, volō); `ut` / `nē` (subjunctive clause); `quod`; `impers:dat+inf`
(licet); `impers:acc` (paenitet); `prep:in+abl`, `prep:ad+acc`, `prep:dē+abl`, `prep:cum+abl`, `prep:ā+abl`,
`prep:ex+abl`, `prep:in+acc`; `intr` intransitive; `copula`; `refl` reflexive (sē). Several frames separated by `;` in
preference order.
Pattern syntax in `phrasebook_en_la.tsv`: lower-case lemmatised words; `{NP}` `{NAME}` `{VP}` `{ADJ}` `{NUM}` slots;
optional tokens in `(...)`; alternatives `a|b`. Latin side: slot references `{1}`, with case `{1:acc}`, `{1:voc}`;
`/` separates masculine/feminine forms chosen from the speaker glossary (default first).

`tiers_la.tsv` details (C2b). The `key` may end in a homograph digit (`sero2` = serō "sow", next to `sero` = sērō adv
"late"): the digit is dropped from the key and the row applies only to a lemma of the same `pos`. At run time the
tier of this file wins over the lexicon's own tier for key + pos. The `note` column is read as English glosses
(comma- or semicolon-separated items; parentheses and a leading "the/a/an/to" are ignored): a source word listed there
gets that lemma as a candidate even when the lexicon's reverse index lacks it, with a bonus (fidelity 1: +0.1,
2: +0.5, 3: +1.0). Example: `fovea ... hole, pit` makes "hole" -> fovea. Write only real glosses in the note.

`phrasal_en_la.tsv`: English verb lemma, particle (`-` = the bare verb with a fixed Latin frame), Latin verb, frame:
empty (the verb as it is), `refl` (adds the reflexive object: "Everyone bow!" -> Omnēs, inclīnāte vōs!), `intr`,
`acc` / `dat` / `abl` (case of the object noun).

`verbprep_en_la.tsv`: English verb lemma, preposition (canonical English: the Spanish frame builder maps a -> to,
en -> in, de -> of), Latin verb (`-` keeps the verb's own translation), frame: `obj` (the PP's noun becomes the direct
object: "wait for me" -> exspectā mē), `pp` (the PP is translated as usual: "live in" -> habitō in + abl),
`prep:<latin>+<case>` (the verb fixes the Latin preposition: "depend on" -> pendeō ex + abl).

`states_en_la.tsv`: source (an English or Spanish adjective lemma, or verb + noun such as `tener miedo`), Latin, kind:
`verb` (the Latin verb replaces be + adjective: "I am afraid" -> timeō; the "of" phrase becomes its object) or `adj`
(this Latin adjective is the predicate: "The teacher is tired" -> Magister fessus est). Used only when the subject is a
person.

`macron_overrides.tsv`: lemma key, stem as the lexicon writes it, stem as the readers write it (`narro narr nārr`,
`thea the thē`); applied to every form of the lemma, also capitalised, before the macrons option.

Phrasebook additions (C2b): register `vp` rows ("play cards" -> chartīs lūdere) are verb phrases inside a clause: the
row starts at the clause's verb, the last Latin word is the infinitive (the verb, inflected as the clause needs) and
the words before it are written as they are. Slot `{WH}`: a wh word and the rest of its clause, realised as an
indirect question (`that depends on {WH}` -> `id pendet ex eō {1}`; `{1:subj}` asks for the subjunctive). A one-word
`state` row of an adjective ("impossible" -> fierī nōn potest) is also used for the attributive adjective as a
relative clause ("sex rēs quae fierī nōn possunt").
