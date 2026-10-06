# engine/nlp — EN/ES tokeniser, tagger, parser (target `vp_nlp`)

Source analysis for the rule engine (DESIGN.md 10.1 step 2-3, contract in section 17). Offline, deterministic,
no exceptions across the boundary (`vp::Result`), models memory-mapped through `vp::MappedFile`.

Public API: `include/vp/nlp.h`
- `Tokenizer(Lang)` — splits punctuation; keeps contractions (don't, I'm, 'em, goin'), hyphenated words, numbers
  with separators, abbreviations (Mr., U.S., Sra.), URLs, e-mails, "..." whole; ¿ ¡ « » “ ” … — are tokens.
- `Tagger::open(path)` / `tag(tokens)` — UPOS + packed coarse features (`Token::feats`, see `vp::nlp::morph`).
- `Parser::open(path)` / `parse(tokens)` — greedy arc-eager; `head` is 1-based (0 = root), `deprel` universal.
- `Pipeline::open(lang, tag, dep)` / `analyse(text)` — tokenise + tag + parse + lemma. `setLemmatizer(fn)` plugs
  the lexicon lemmatiser (`fn(lower, upos)`; "" or a throw falls back to `ruleLemma`: English regular
  inflection + a few irregulars, PROPN keeps its form, Spanish = lower-cased form).

## Model files (`.vpt`, written by `tools/train/vpt.py`)
Little-endian, sections 8-byte aligned. Header 256 bytes: `VPTX`, u16 major 1, u16 minor 0, char[8] lang,
char[8] kind (`tag`/`dep`), u32 n_labels, u32 n_feats, u32 table_size (power of two), u32 reserved (0),
u64 file_size (@40), u8[32] SHA-256 of bytes [256, file_size) (@48), zero padding. Then LABELS (NUL-terminated,
padded to 8), FEATS (table_size x u64 FNV-1a 64 hashes, 0 = empty, linear probing from `hash & (size-1)`),
WEIGHTS (table_size x n_labels x int16), NOTE (licence text, to the end of the file).
- Tagger labels: the UPOS tags, then one head per feature group: `Number=_`, `Number=Plur`, ... (`=_` = absent).
- Parser labels: `@SHIFT @REDUCE @LEFT @RIGHT`, then the deprels. The transition is the best valid one; for an arc
  the label is the best deprel on the configuration features plus the label features (`root` only from the root).
- Scores are int32 sums of int16 weights; ties go to the lowest label index. Feature strings are built by
  `src/feature_strings.cpp`, a line-by-line mirror of `tools/train/features.py`.
- A file with a bad magic, major version, kind, size, layout or SHA-256 gives `ErrorCode::Internal` with the
  hint "model file damaged"; a missing file gives `Io`.

Files: `data/work/nlp/{english,spanish}.{tag,dep}.vpt` (built by `tools/train`, not committed; see
`tools/train/README.md` and `tools/train/report.json` for scores and sizes). Tiny English models for the tests:
`tests/fixtures/nlp/tiny.{tag,dep}.vpt`.

## Tests
`engine/tests/test_nlp.cpp` (doctest, part of `vp_tests`):
`./build/engine/tests/vp_tests -tc="nlp*"`
- feature strings and FNV-1a hashes vs `tests/fixtures/nlp/features_golden.tsv` (500 rows);
- tiny models reproduce `tiny.golden.tsv` (200 EWT dev sentences: UPOS, feats, head, deprel) exactly;
- full models vs `en.golden.tsv` / `es.golden.tsv` when `data/work/nlp/*.vpt` exist and match the SHA-256 in the
  golden header (otherwise skipped with a message);
- tokeniser table (41 cases), corrupt/truncated files, lemma rules and hook, RSS flat over 10,000 sentences.
Regenerate the golden files with `tools/train/make_golden.py` after any retraining.
