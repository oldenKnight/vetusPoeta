# tools/eval

Error-measurement harness (task C10). How to run and how the numbers are defined: `docs/EVAL.md`.

| File | What |
|---|---|
| `evallib.py` | `vpengine serve` client, gold normalisation, Clopper-Pearson, McNemar, kappa, held-out guard, export identity check |
| `run_eval.py` | combination x fidelity matrix over one subtitle file -> report.json, report.md, cues_*.jsonl; `--dump-source`; `--compare-system` |
| `review_sheet.py` | expert review CSV export (flagged + seeded sample, blind A/B) and import (rates, rule of three, kappa) |
| `gt_compare.js` | Google Translate web page batches via Playwright (dev only; output stays in data/work/gt/) |
| `templates/compare.html` | side-by-side page template |
| `tests/test_evallib.py` | `python3 -m unittest discover -s tools/eval/tests` |
| `make_heldout.py` | builds the held-out SRT from a public-domain text (frozen set in tests/heldout) |
| `llm_gate.py` | local-model minimal-pair gate (task C5) |
