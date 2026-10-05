# HANDOFF — read on restart

Last updated: 2026-10-05 (session 1, step 1 of the production process).

## Where we are
- Owner approved the plan ("start"). Decisions in `docs/DECISIONS.md`. Ledger in `docs/STATUS.md`.
- Repo was empty; skeleton committed. No code yet.
- Prototype repo BabyDaVinci is cloned at `/home/user/babydavinci` (re-clone with
  `git clone --depth 1 https://github.com/oldenKnight/babydavinci /home/user/babydavinci` if
  missing). Reuse: shell (gui/shell), engine_bridge.js, i18n pattern, project.cpp (zip,
  atomic save, autosave, lock/recovery), server JSON-lines protocol, tools/xcompile_check.sh,
  tools/pack_ui.py, tools/make_dist.py.
- Network: environment allowlist is open (kaikki.org, wiktionary.org, dumps.wikimedia.org,
  latinitium.com, translate.google.com, huggingface.co, raw.githubusercontent.com reachable).
  Wiktionary API rate-limits bare requests: send a descriptive User-Agent and throttle.
- Raw data downloads go to `data/raw/` (gitignored). Sizes: Latin 1.23 GB, Ancient Greek
  404 MB, English 3.3 GB, es.wiktionary extract 103 MB gz. If `data/raw/dl-done.flag` is
  missing, check `data/raw/dl-*.log` and rerun the curl commands (they resume with `-C -`).

## Next step
- M0.3: Sonnet pre-plan. Then M0.4: main agent writes DESIGN.md with [CONTRACT] sections and
  opens implementation tasks (M1, M2) for Opus implementers.

## Open items for the owner
- Upload the Alice subtitle file (acceptance test) and a second subtitle file (held-out test).

## Measured data facts (2026-10-05, full passes over the raw dumps)
- Latin (en.wiktionary via Kaikki): 892,320 entries; 54,199 lemma entries; 838,121 form-of
  entries; 61,224 entries with inflection tables; 2,051,746 table forms. Macrons live in
  `forms[].form` and `head_templates[].expansion`, not in `word` (only 7 words carry one).
- Ancient Greek: 68,196 entries; 23,169 lemmas; 1,465,603 table forms. Verb tables use
  `forms[].source == "inflection"` (not "conjugation") and carry dialect variants (Attic,
  Ionic, Epic): filter by dialect tag when building Attic paradigms. Contract verbs list
  uncontracted forms in the non-Attic tables.
- English: 1,492,836 entries; 18,774 distinct English words have a Latin translation
  (20,003 entries), 11,543 entries have an Ancient Greek translation.
- Spanish Wiktionary extract: only 7,022 Latin and 278 Greek entries, so Spanish glosses come
  mainly from the English pivot and the hand-written tier vocabulary (decision D8).
- Treebank licences: UD_English-EWT, UD_Spanish-GSD, UD_Spanish-AnCora, UD_Latin-LLCT,
  UD_Latin-CIRCSE are CC BY(-SA) and usable; UD_English-GUM and all Perseus/PROIEL/ITTB Latin
  and Greek treebanks are CC BY-NC-SA and must not be used.
- No pull request yet: the repo has no default branch besides ours, so there is no base to
  open a PR against. The owner should create `main` (or say which base to use).
