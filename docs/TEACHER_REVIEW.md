# Reviewing the vocabulary tiers (for the teacher)

The translator prefers words by **tier**: tier 1 = the beginner core (Familia Romana / Athenaze level), tier 2 = common
classical vocabulary, tier 3 = everything else in the dictionary. "Extremely faithful" may use any tier; "flexible"
stays inside tiers 1-2 whenever the meaning allows. Your lists decide tier 1 and tier 2.

## 1. Look at the review sheets
Each library build writes two spreadsheets (open them in Excel or LibreOffice; UTF-8, comma-separated):

* `data/work/next/review_tier_sheet_la.csv` (Latin), `data/work/next/review_tier_sheet_grc.csv` (Greek)
  (after the build is promoted they are regenerated in `data/work/` by the next build).

| Column | Meaning |
|---|---|
| key | the spelling the program matches on (no macrons, u for v, i for j; Greek with accents, σ for ς) |
| head | the dictionary headword as shown to students |
| pos | part of speech (noun, verb, adj, adv, prep, conj, pron, particle, ...) |
| tier | 1 or 2 (rows marked `candidate` are tier 3 today) |
| source | why the word has this tier: `teacher` (your list), `curated` (the project's starter list), `dcc` (Dickinson College core list), `dcc+el` (Greek core word still used in Modern Greek), `whitaker` (Whitaker's Words marks it very frequent), `candidate` (Latin, tier 3, but Whitaker marks it frequent: consider promoting it) |
| dcc_rank | rank in the Dickinson College Commentaries core list (1 = most frequent), empty if not listed |
| whitaker_code | Whitaker's frequency letter: A very frequent, B frequent, C common, D lesser, E uncommon, F very rare |
| gloss_en, gloss_es | the short English / Spanish glosses the program shows |
| lemma_id | internal number (ignore it) |

You do not edit the sheets themselves: they are rebuilt every time. Write your decisions into the tier files below.

## 2. Edit the tier files
`data/curated/tiers_la.tsv` and `data/curated/tiers_grc.tsv` are plain text with one word per line and **tab**
characters between the columns (a spreadsheet saved as "tab-separated text" works; keep UTF-8):

```
# key	head	pos	tier	source	note
puella	puella	noun	1	teacher	girl
ambulo	ambulō	verb	1	teacher	walk
eo	eō	adv	1	teacher	thither
```

* **key**: copy it from the sheet. **head**: the headword with macrons (Latin) or accents (Greek), as in the sheet.
* **pos**: as in the sheet; it tells two words with the same spelling apart (`eo` verb "go" and `eo` adverb).
  For Latin homographs of the same part of speech a digit may follow the key (`sero2`).
* **tier**: `1` or `2`. A word you do not list keeps the tier the program derives (see `source`).
* **source**: write `teacher` on every line you have checked; `derived` lines are the project's guesses.
* **note** (optional): English glosses separated by commas. A source word listed here makes the program consider
  this Latin word for it even when the dictionary's index misses it ("hole, pit" on `fovea`). Write real glosses only.
* Lines starting with `#` are comments. Each word only once (the build warns about duplicates).

To demote a word, change its tier or delete its line. To promote a `candidate`, add a line with tier 1 or 2.

## 3. Rebuild and check
Ask the developer (or run) the library build: `python3 tools/build_library/build.py --raw data/raw --out data/work
--next --stage tiers,pack` rebuilds the tiers and the lexicon files into `data/work/next/` and rewrites both review
sheets; check that your words show `teacher` in the `source` column. The words of your list that the dictionary could
not find are listed in `data/work/next/<la|grc>/tiers.json` under `curated_unmatched` (usually a typo in the key or
a wrong part of speech).
