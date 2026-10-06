# tools/train — EN/ES tagger and parser trainers (Python 3.11, stdlib only)

Trains the models read by `engine/nlp` (format and protocol: DESIGN.md section 17). Outputs go to `data/work/nlp/`
(gitignored); the measured scores, sizes, times and data SHA-256s go to the committed `report.json`.

## Data and licences
`data/raw/ud/` (gitignored): `en_ewt-ud-{train,dev,test}.conllu` (UD_English-EWT, CC BY-SA 4.0),
`es_gsd-ud-*.conllu` (UD_Spanish-GSD, CC BY-SA 4.0), `es_ancora-ud-*.conllu` (UD_Spanish-AnCora, CC BY 4.0).
Spanish models train on GSD + AnCora train concatenated; Spanish dev/test = both dev/test sets. The models are
derived works and carry the attribution in their NOTE section; ship them under CC BY-SA 4.0. Never use
UD_English-GUM or other CC BY-NC(-SA) treebanks (docs/HANDOFF.md).

## Retrain (from the repository root)
```
python3 tools/train/train_tagger.py --lang en --data data/raw/ud --out data/work/nlp/english.tag.vpt --eval-float
python3 tools/train/train_tagger.py --lang es --data data/raw/ud --out data/work/nlp/spanish.tag.vpt --eval-float
python3 tools/train/train_parser.py --lang en --data data/raw/ud --out data/work/nlp/english.dep.vpt \
    --tagger data/work/nlp/english.tag.vpt --eval-float
python3 tools/train/train_parser.py --lang es --data data/raw/ud --out data/work/nlp/spanish.dep.vpt \
    --tagger data/work/nlp/spanish.tag.vpt --eval-float
python3 tools/train/common.py          # adds the per-language "summary" section to report.json
python3 tools/train/make_golden.py --lang en --tag data/work/nlp/english.tag.vpt \
    --dep data/work/nlp/english.dep.vpt --out tests/fixtures/nlp/en.golden.tsv      # same for es
```
The defaults are the settings recorded in `report.json`: 10 epochs, seed 7, 2^16-slot tables filled to 80 %,
parser with a dynamic oracle (exploration 0.9 from epoch 2), training UPOS jackknifed over 4 folds (5-epoch
taggers), and a second pass restricted to the exported features (both trainers). `--eval-float` adds the
unpruned and first-pass scores (the cost of pruning). The parser needs its tagger only for evaluation, so all four
runs can start together if each tagger finishes before its parser evaluates (the parser log prints the tagger
SHA-256 at every evaluation). Wall times with the four running side by side on 4 cores: English tagger ~5 min,
Spanish tagger ~25 min, English parser ~18 min, Spanish parser see `report.json` (`total_seconds`).
After retraining, regenerate the golden files; the C++ test skips the full-model check (with a message) when
the model SHA-256s differ from the golden header.

Tiny test models (committed, `tests/fixtures/nlp/`, first 200 EWT train sentences):
```
python3 tools/train/train_tagger.py --lang en --out tests/fixtures/nlp/tiny.tag.vpt --epochs 5 --max-sents 200 --table-bits 11 --no-report
python3 tools/train/train_parser.py --lang en --out tests/fixtures/nlp/tiny.dep.vpt --tagger tests/fixtures/nlp/tiny.tag.vpt \
    --epochs 5 --max-sents 200 --table-bits 11 --no-report
python3 tools/train/make_golden.py --lang en --tag tests/fixtures/nlp/tiny.tag.vpt --dep tests/fixtures/nlp/tiny.dep.vpt \
    --out tests/fixtures/nlp/tiny.golden.tsv --features tests/fixtures/nlp/features_golden.tsv
```

## Files
- `conllu.py` reader (syntactic words; multiword ranges and empty nodes skipped; deprel subtypes collapsed) and
  projectivisation by lifting (Nivre and Nilsson 2005, no label encoding).
- `features.py` feature strings + FNV-1a 64; mirrored by `engine/nlp/src/feature_strings.cpp`.
- `perceptron.py` averaged perceptron with label heads, feature counts, pruning (count >= 2, all-zero rows,
  then the largest-L1 rows up to 80 % of the table), int16 quantisation (`round(w * 256)` after rescaling so the
  largest weight fits).
- `train_tagger.py` UPOS head + feature heads (Number, Person, Tense, VerbForm, Mood, PronType); greedy.
- `train_parser.py` arc-eager (static oracle or dynamic oracle with exploration), factored transition / label
  heads, single root, optional jackknifed training tags, second pass restricted to the exported features.
- `vpt.py` .vpt writer/reader; `make_golden.py` golden files; `common.py` data sets, NOTE text, report merging.
