# tools/build_library (LIB)
Python 3.11, stdlib only. Raw Wiktionary dumps (`data/raw/`, gitignored) -> intermediate files (`data/work/`) ->
later stages -> `.vpl` lexicons (DESIGN.md section 5). Stages built so far: `fetch`, `kaikki`, `resolve`.

    tools/build_library/fetch.sh [fetch|check|record|dates]     # downloads, SHA256SUMS, SOURCES.json (dump dates)
    python3 tools/build_library/build.py --raw data/raw --out data/work --lang la,grc,en,es --stage kaikki,resolve
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
