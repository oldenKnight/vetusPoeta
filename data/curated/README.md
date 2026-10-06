# Curated data (hand-written, part of the proprietary program; tab-separated, UTF-8, `#` comments allowed)

All files are written by the project itself (main agent and teacher); nothing here is copied from a textbook or a
dictionary. Latin headwords carry macrons; the `key` column is `latin_key(head)` (DESIGN §4). Lines starting with `#`
and blank lines are ignored by every loader. Loaders live in tools/build_library (build time) and engine/rules
(runtime copies are embedded into the lexicon or shipped as `data/*.tsv` next to the engine; decided per file below).

| File | Columns | Used by |
|---|---|---|
| `tiers_la.tsv` | key, head, pos, tier (1/2), source (derived/teacher), note | build: LEMM.tier; engine A6 |
| `valency_la.tsv` | key, frame, example, note | engine transfer/realise (A4) |
| `preps_en_la.tsv` | english, context, latin, case, note | engine transfer |
| `contractions_en.tsv` | form, expansion | engine nlp |
| `phrasebook_en_la.tsv` | pattern, latin, tier, register, note | engine frame pre-pass |
| `names_la.tsv` | english, latin_nom, latin_gen, gender, declension, policy, note | engine names seed |
| `emoji_la.tsv` | key, head, emoji, note | build: LEMM.emoji; engine display |
| `nonverbal_en_la.tsv` | english, latin | engine cue assembly |
| `periphrasis_la.tsv` | key, tier, periphrasis, note | engine transfer (fidelity 2/3) |
| `gloss_es_la.tsv` | key, head, gloss_es | build: LEMM.gloss_es, SENS |

Frames in `valency_la.tsv`: `acc` direct object accusative; `dat` dative object; `abl` ablative object; `gen` genitive
object; `dat+acc` (dare); `acc+inf`; `inf` (possum, volō); `ut` / `nē` (subjunctive clause); `quod`; `impers:dat+inf`
(licet); `impers:acc` (paenitet); `prep:in+abl`, `prep:ad+acc`, `prep:dē+abl`, `prep:cum+abl`, `prep:ā+abl`,
`prep:ex+abl`, `prep:in+acc`; `intr` intransitive; `copula`; `refl` reflexive (sē). Several frames separated by `;` in
preference order.
Pattern syntax in `phrasebook_en_la.tsv`: lower-case lemmatised words; `{NP}` `{NAME}` `{VP}` `{ADJ}` `{NUM}` slots;
optional tokens in `(...)`; alternatives `a|b`. Latin side: slot references `{1}`, with case `{1:acc}`, `{1:voc}`;
`/` separates masculine/feminine forms chosen from the speaker glossary (default first).
