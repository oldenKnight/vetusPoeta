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
