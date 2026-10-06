# tools/build_library (LIB)
Python 3.11, stdlib only. Raw Wiktionary dumps (`data/raw/`, gitignored) -> intermediate files (`data/work/`) ->
`.vpl` lexicons (DESIGN.md section 5). Stages: `fetch`, `kaikki`, `resolve` (A4), `import_aux`, `gloss`, `tiers`,
`pack` (B4).

    tools/build_library/fetch.sh [fetch|check|record|dates]     # downloads, SHA256SUMS, SOURCES.json (dump dates)
    tools/build_library/fetch.sh aux                            # Whitaker, Perseus LS + LSJ, DCC -> data/raw/aux
    python3 tools/build_library/build.py --raw data/raw --out data/work --lang la,grc,en,es --stage kaikki,resolve
    python3 tools/build_library/build.py --raw data/raw --out data/work --lang la,grc,en,es \
        --stage import_aux,gloss,tiers,pack                     # -> data/work/{latin,greek,english,spanish}.vpl
    python3 tools/build_library/vpl_inspect.py data/work/latin.vpl puellae [--reverse water] [--validate] [--sha]
    python3 tools/build_library/tests/compare_spec.py           # pack.py vs B1's reference encoder bytes
    python3 tools/build_library/tests/make_vpl_spec.py [--check] # hand-made inputs + tiny .vpl in tests/fixtures/vpl_spec
    python3 tools/build_library/build.py --out data/work --check     # drift rule vs expected_counts.json
    python3 tools/build_library/build.py --out data/work --report    # prints data/work/report.json
    python3 -m unittest discover -s tools/build_library/tests        # tests (hand-made fixtures, no raw data)
    python3 tools/build_library/make_golden.py [--check]             # regenerate / verify the golden TSVs

Stages cache: `<out>/<lang>/<stage>.json` holds input SHA-256s, a hash of the stage code, output sizes, counts,
duration and peak RSS; a stage is skipped when nothing changed (`--force` reruns). `--check` fails when a count in
`expected_counts.json` moves > 15 % or disappears (`--write-expected` rewrites it from the current run).
A `/usr/bin/time -v` log saved as `<out>/<lang>/time-v.txt` is copied into report.json.

## Contracts
* `vptext.py` = DESIGN section 4; `tests/fixtures/normalisation_golden.tsv` rows are `function<TAB>input<TAB>expected`,
  functions `nfc latin_key greek_key greek_bare en_key es_key es_bare display_latin_plain display_latin_macrons`
  (the last two are `display_latin(form, false/true)`). `es_bare` strips U+0300-U+036F, so n-tilde -> n.
* `features.py` = FEAT bit layout (do not edit). `tagmap.py` maps Kaikki tags to packed values.
  `tests/fixtures/features_golden.tsv` rows are `pos=<POS name> <sorted Kaikki tags><TAB><packed decimal>`.
  Mapping choices: past -> perfect; past+imperfect -> imperfect; present+perfect -> perfect; future+perfect ->
  future-perfect; mediopassive or middle+passive -> two analyses; several cases/numbers/persons -> one analysis each;
  genders combine (m+f -> mf); adverbial -> pos adv; Attic -> extra attic; supine/gerundive/contracted/alternative ->
  extra bits. Lossy tags (no enum slot: conditional, potential, sigmatic, vos-form...) and unknown tags are counted
  in report.json; capitalised tags are region labels and only counted.

## Files in `<out>/<lang>/`
* `lemmas.jsonl`: id (sequential), word, key, head (display with length marks), pos (Kaikki), fpos (features POS),
  ht (head template), kind (lemma | formtable | alttable), has_table, gender, class, pp (head-line expansion),
  senses [{g last-level gloss, tags, raw_tags, q qualifier, topics, ex examples count}] (la, grc; en/es carry `ns`),
  of (targets of formtable/alttable), late (la: every sense Medieval/Late/New Latin), el (grc: Modern Greek
  descendant, 1 = same spelling under greek_bare, 0 = different).
* `table_forms.tsv`: lemma_id, display, key, tags, source (declension | conjugation | inflection | head), marker
  (table-tags row text, e.g. "Attic declension-2", "contracted present"). Head-line forms: la/grc only alternative
  spellings (or all when there is no table); en/es all. Multi-word forms are kept for la/grc, skipped for en/es.
* `formpages.tsv`: word, key, display, target_key, tags, pos, kind (form | alt), target as written.
* `analyses.tsv`: key, lemma_id, packed, display, flags (bit0 table, bit1 form page, bit3 alternative, bit4
  non-Attic, bit5 Medieval/Late/New Latin, bit6 poetic/rare/archaic); sorted, unique; provenance bits OR-ed,
  qualifier bits AND-ed when rows merge. Form pages follow form-of chains to depth 3.
* `lemma_index.tsv`: lemma_id, key, head, pos, has_table, kind, ht.  `report.json`: counts, histograms, tags, time.
* en: `translations_{la,grc,es}.tsv` (English word, pos, sense text, target, tags; entry- and sense-level).
  es: `latin_glosses.tsv` (word, pos, gloss, lang, sense_index, form_of), `translations_{la,grc}.tsv`.

## Greek tables
Rows are grouped by their marker. No dialect or Attic -> kept (extra attic bit when named); other dialects -> bit4.
Verb cells carry no tense: it comes from the marker. A table that has a `contracted` twin (present / contracted
present) is the uncontracted, non-Attic one -> bit4; contracted tables set the extra contracted bit.

## B4 stages (import_aux, gloss, tiers, pack)
`import_aux`, `gloss` and `tiers` run for la and grc; `pack` for all four languages. The B4 stages run in a child
interpreter (`build.py` `run_isolated`), so the `peak_rss_mb` in `<stage>.json` / report.json is that stage's own.

### Auxiliary sources (`fetch.sh aux` -> `data/raw/aux/`, SOURCES.json with url, fetch date, size, sha256, licence)
| Source | Files | Licence (verified 2026-10-06) |
|---|---|---|
| Whitaker's Words (mk270/whitakers-words, master) | `whitaker/DICTLINE.GEN`, `INFLECTS.LAT`, `ADDONS.LAT`, `UNIQUES.LAT`, `README.md`, `LICENCE.txt`, the two Ada specs that define the code letters | permission text (quoted below) |
| Perseus Lewis & Short | `perseus/lat.ls.perseus-eng1.xml` | CC BY-SA 4.0 (PerseusDL/lexica README) |
| Perseus LSJ | `perseus/grc.lsj.perseus-eng1.xml` .. `eng27.xml` | CC BY-SA 4.0 |
| DCC Latin core (997), Latin core Spanish, Greek core (524 rows) | `dcc/latin-core-list.csv`, `latin-core-list-es.csv`, `greek-core-list.csv` (served as `text/csv`) | CC BY-SA 3.0 Unported (dcc.dickinson.edu/vocab/core-vocabulary); lists by a Dickinson College team led by Christopher Francese; Latin-Spanish translation credited there to Francisco Javier Pérez Cartagena |

Whitaker's permission (README.md "Licensing" and LICENCE.txt): "This is a free program, which means it is proper to
copy it and pass it on to your friends. Consider it a developmental item for which there is no charge. However, just
for form, it is Copyrighted (c). Permission is hereby freely given for any and all use of program and data. You can
sell it as your own, but at least tell me. [...] All parts of the WORDS system, source code and data files, are made
freely available to anyone who wishes to use them, for whatever purpose." The repository has no docs folder: the
letter codes are defined in `src/latin_utils/latin_utils-inflections_package.ads` (Age_Type, Frequency_Type,
Noun_Kind_Type, Verb_Kind_Type) and `latin_utils-dictionary_package.ads` (Area_Type, Geo_Type, Source_Type); both are
downloaded next to the data. Meanings as written there:
* **age** X in use throughout the ages / unknown (default); A archaic (very early, obsolete by classical times);
  B early (pre-classical, used for effect/poetry); C classical (~150 BC - 200 AD); D late (3rd-5th c.);
  E later (6th-10th c., Christian); F medieval (11th-15th c.); G scholar (post-15th, 16th-18th c.); H modern (19th-20th c.).
* **frequency** X unknown; A very frequent ("in all elementary Latin books"); B frequent ("top 10 percent");
  C common ("top 10,000 words"); D lesser ("top 20,000 words"); E uncommon ("2 or 3 citations"); F very rare
  ("single citation in OLD or L+S"); I inscription only; M graffiti; N Pliny ("almost only in Pliny Natural History";
  not "proper name" as PREPLAN 1.5 guessed).
* **area** X all/none; A agriculture, flora, fauna; B biological, medical, body; D drama, music, art; E ecclesiastic,
  biblical; G grammar, rhetoric, logic, schools; L legal, government, financial; P poetic; S science, philosophy,
  mathematics; T technical, architecture; W war, military, naval; Y mythology.
* **geo** X all/none; A Africa; B Britain; C China; D Scandinavia; E Egypt; F France/Gaul; G Germany; H Greece;
  I Italy/Rome; J India; K Balkans; N Netherlands; P Persia; Q Near East; R Russia; (more in the Ada spec).
* **source** X general/unknown; B Beeson; C Cassell's; E Stelten; G Gildersleeve+Lodge; L Lewis Elementary;
  O Oxford Latin Dictionary; S Lewis & Short; W Whitaker's own guess; ... (full list in the Ada spec).
* noun kinds (4th grammar code of N): N proper name, L locale, G group name, P person, T thing, A abstract, W place.

### `import_aux` outputs
* `la/whitaker.tsv`: line, stem1..stem4 (`zzz` -> empty), pos, codes (declension/conjugation, variant, gender/kind),
  age, area, geo, freq, source, meaning, cite_key, lemma_ids. Column layout of DICTLINE.GEN: stems at 0/19/38/57
  (19 wide), POS at 76, codes 83-99, letters at 100/102/104/106/108, meaning from 110. Join: citation forms are
  generated from stem1 and the POS/declension/conjugation (N 1 -> -a, N 2 -> -us/-um, N 3 -> stem1, V 1 -> -o,
  V 2 -> -eo, V 3 -> -o/-io, deponents -or, ADJ 1 1 -> -us, ADJ 3 2 -> -is, ...) and the first whose `latin_key`
  is a Kaikki lemma of a compatible POS wins; homographs are split by the other stems found in the Kaikki head
  line (vol/vell/volu -> volō "want", not volō "fly"); then alternative-spelling form pages (adcessus -> accessus).
  PACK lines are not joined. Pronouns / sum / nōn are not in DICTLINE (hard-coded in Whitaker's program).
* `la/whitaker_inflects.tsv`: pos, grammar, stem_key, ending_len, ending, age, freq (INFLECTS.LAT, comments dropped).
* `la/ls.tsv`: key (`key` attribute, homograph digits stripped, `latin_key`), hom, type (main/greek/hapax/gloss/
  spur/foreign), orth (first `<orth>`, length marks kept, `^`/`_` and prefix hyphens removed), itype, gen, pos,
  tr (up to 5 short `<tr>`, `; `), def (first italic text of the first sense), usg labels, lemma_ids.
* `grc/lsj.tsv`: key (`greek_key` of the Beta Code key converted by `betacode.py`), bare (`greek_bare`), hom, beta,
  orth, gen, tr, lemma_ids, match (exact | bare = unambiguous match without accents).
* `la/dcc_la.tsv`, `la/dcc_la_es.tsv`, `grc/dcc_grc.tsv`: key, head (first headword token), rank, definition, pos,
  semantic group, full headword, lemma_ids (enclitics `que` -> `-que`, plural headwords singuli -> singulus).
The XML is read with `xml.etree.iterparse` (expat never fetches the external DTD) and each entry is cleared.

### `gloss` outputs (la, grc)
* `gloss.tsv`: lemma_id, gloss_en, gloss_en_src (kaikki | of | ls | whitaker | lsj), gloss_es, gloss_es_src
  (curated | dcc | eswikt | pivot), n_senses. gloss_en = first sense, parentheticals removed, cut at `;` or the last
  `, ` before 60 characters (else at a space + `…`); verb glosses keep Wiktionary's `to `. Form/alt-table lemmas
  (participles, alternative spellings) take the gloss of the lemma they point to (`of`).
  gloss_es priority: `data/curated/gloss_es_la.tsv` > DCC Spanish > es.wiktionary Latin entries > EN->ES pivot
  (up to three English head words of gloss_en -> each one's most common Spanish translation in the English
  entries, same POS first). Pivot glosses set LEMM flags bit8 in the .vpl so the UI can say "(via English)".
* `senses.tsv`: lemma_id, sense_idx, gloss_en (<= 200 chars), gloss_es (sense 0 only), keywords, tags, rank.
  Tags (SENS bits 0-14) from Kaikki tags / raw_tags / qualifier (transitive, figuratively, rare, obsolete -> archaic,
  poetic, Medieval/Late/Ecclesiastical Latin -> Medieval, New/Renaissance Latin -> New Latin, with-dative, ...,
  Greek Epic/Homeric -> poetic) plus the frames of `data/curated/valency_la.tsv` (acc -> transitive + with-acc,
  dat, abl, gen, inf, acc+inf, intr, refl, impers:...).
* `revx_en.tsv`, `revx_es.tsv`: keyword, lemma_id, sense_idx, score, pos. Keywords are `en_key` / `es_key` of
  lemmatised gloss words (`stopwords_en.txt`, `stopwords_es.txt`; English/Spanish forms -> lemma through
  `data/work/{en,es}/analyses.tsv`, a word that is itself a lemma stays, unknown words through suffix rules).
  Score (DESIGN 5.2, integer hundredths): +100 exact translation (EN: English entries' Latin/Greek translations;
  ES: es.wiktionary translations and the curated Spanish glosses), +60 head word of the lemma gloss (first content
  word of each comma/semicolon segment of the first sense, of the Whitaker meaning and of the DCC definition),
  +35 another content word or a head word of a later sense (the larger of the two), +15 Whitaker A/B, +10 DCC,
  +20 tier 1, -30 sense (or translation row) tagged rare/archaic/poetic/Medieval/New Latin, -20 proper name with a
  keyword written lower-case; clamp 0..255. One candidate per (keyword, lemma): its best sense. Pivot-derived
  Spanish words count +35 only. A negated segment ("no querer") gives no head word. Homograph pivot targets are
  narrowed to the lemmas whose glosses contain the English word.
* **Spanish keywords are stored in the one REVX section with the prefix `es:`** (`es:agua`), so that an English and
  a Spanish keyword with the same spelling (`red`, `pan`, `once`) never share a candidate list. The engine calls
  `reverse("es:" + es_key(word))` for Spanish sources (recorded in docs/STATUS.md for the main agent).

### `tiers` outputs (la, grc)
* `tiers.tsv`: lemma_id, key, head, pos, tier, tier_source, freq_rank, whit_freq, dcc_rank, emoji, shared_el.
  Latin: tier 1 = `data/curated/tiers_la.tsv` (tier_source derived 1 / teacher 2; ties between homographs go to the
  DCC member, then the better Whitaker letter); tier 2 = DCC Latin core + Whitaker frequency A/B with age X or C
  joined to a Kaikki lemma with a table (proper names excluded: Kaikki POS name, capitalised head, Whitaker noun
  kinds N/L/G); tier 3 = the rest; 0 = no gloss at all. Greek: tier 1 = DCC Greek lemmas with `el` = 1 (Modern
  Greek descendant spelled the same) + `data/curated/tiers_grc.tsv` (20-word seed); tier 2 = rest of DCC Greek.
  freq_rank = DCC rank, else Whitaker bucket A 1000, B 2000, C 3000, D 4000, E 5000, F/I/M/N 6000, else 0.
* `freq.tsv`: lemma_id, freq_rank, source, dcc_rank, whit_freq.  `review_tier_sheet.csv` (for the teacher): key,
  head, pos, tier, tier_source, dcc_rank, whitaker_freq, gloss_en, gloss_es, lemma_id for tiers 1 and 2.
* Emoji: `data/curated/emoji_<lang>.tsv` joined by key (nouns); unmatched keys are listed in tiers.json/report.

### `pack` (`data/work/{latin,greek,english,spanish}.vpl`)
Byte conventions of B1's reference encoder (engine/tests/lex_fixture_writer.cpp, tests/fixtures/lex/SPEC_CHECK.md;
`tests/compare_spec.py` reproduces all three fixtures byte for byte). LEMM: head (display with length marks), key,
pos (features.POS of the Kaikki POS), cls (declension/conjugation number 1-5, else 0), gender (features.GENDER,
m+f -> mf), tier, freq_rank, whit_freq ('A'..'F' or 0), tier_source, emoji, gloss_en/es, senses, flags (bit0 name,
bit1 indeclinable, bit2 deponent, bit3 impersonal, bit4 shared_el, bit5 plural-only, bit6 defective, bit7
has_table, bit8 gloss_es via the English pivot), GENX range, principal = the Wiktionary head line (count 1).
ANAL = analyses.tsv, except that an analysis whose display is the lemma's headword or canonical word loses flag
bit3/bit4. GENX: one display form per (lemma, feature word): table rows before head-line rows, then the form without
alternative / non-Attic / Late / rare flags, then table order; the other forms stay as ANAL rows.
English and Spanish files: NOTE STRS KEYS ANAL LEMM FEAT only; multi-word lemmas (phrases) are left out and the
lemma ids renumbered densely. NOTE carries the licences above and the Kaikki dump dates from data/raw/SOURCES.json.
