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

## Next step (updated 2026-10-06, evening)
- 77 tasks DONE through C35 / R8 (Latin loops 1-5, two acceptance loops, Greek loops 1-5, Orbergise on real material,
  latinity toggle D18, Latin loops 6-8, Greek loops 6-8, Spanish-source loops C34 (Latin) and C35 (Greek), release passes R1-R8). No implementer running; every commit is pushed to
  claude/fervent-cannon-6qif8i. The tree is release-ready on Linux; nothing engineering-side is pending that does not
  need the owner's input (second file, main branch, Google Translate run, Windows smoke test).
- Waiting on the owner: the Alice .srt (acceptance run: `tools/eval/run_eval.py --tuned`, then a tuning loop C18 on
  its first-run mismatches, then the true held-out file), a `main` branch for the draft pull request, the Google
  Translate side-by-side on his machine (docs/EVAL.md §5), the Windows smoke checklist (docs/BUILD.md).
- Next engineering candidates if time remains before the files arrive: Greek loop 3 (docs/rules_grc2_notes.md "After
  B4c"), LIB-5 (duplicate caelum lemmas 1599/1600 in latin.vpl, tier review sheets), the 27 remaining oz_sample
  mismatches (docs/rules_en_notes.md "Quality loop 3").
- Review protocol: `git archive HEAD | tar -x -C <scratch>` then build + ctest + xcompile there, so other
  implementers' uncommitted files do not pollute the verification. Commit the ledger with pathspecs
  (`git commit docs/STATUS.md -m ...`) so staged files of implementers are not swept in.
- Gold Latin: tests/regression/expected/*.gold.txt (main agent). Held-out hygiene: nobody reads tests/heldout,
  tests/eval_gold or data/work/eval/heldout-*; BURNED.txt lists the 100 cues moved to the tuning sample.
- Quality state (honest): tuned EN 114/114, ES 100/100, Greek 114/114 (ES->GRC 39/40), oz_sample 78/100; held-out
  automatic errors own 2/74 (2.7 %), oz 97/700 (13.9 %); wrong among OK 0 everywhere; model gate 74.9 % so Latin
  reranking is off. Acceptance file (owner's partial Alice, 82 cues): expert wrong rate 79 % at first run, 27 % after
  loop 1 (C22), 3.7 % after loop 2 (C24; tuned number, no < 1 % claim). Latinity toggle (D18) done in UI and engine.
  Next honest test: the owner's second file (held-out, never tuned on).

## Open items for the owner
- The owner's PARTIAL Alice file arrived 2026-10-07 13:13 UTC (82 cues) with his own free Latin version; both are in
  data/acceptance/ (gitignored). First run measured: STATUS E5 (expert wrong rate 79 %). Tuning loop C22 opened.
  The intake plan that was followed: save it under data/acceptance/ (gitignored: the file is copyrighted and never enters the repo or the
  docs; only our own Latin output lines and numbers are recorded), run `vpengine check` on it, then
  `tools/eval/run_eval.py --tuned --file data/acceptance/<file> --pair en-la --combos R,RM,RO,RMO --fidelity 1,2,3`
  (every engine combination, first run, per-cue error with intervals; the online cells only if the owner toggles
  them), write the expert review sheet (docs/EVAL.md §3), review every cue as the four reviewers of DECISIONS D15,
  report the true first-run error rate per cell, then open the tuning loop on its mismatches (C20) and re-measure
  on the held-out files. The partial file gives a wider interval; the protocol is unchanged.
- Create the `main` branch so a draft pull request can be opened for claude/fervent-cannon-6qif8i.
- Run tools/eval/gt_compare.js headful on your machine (the container cannot trust the proxy CA).
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

## Interruption protocol (used 2026-10-06 03:30 UTC after an API session limit)
When implementers die mid-task: `git add` their task paths and commit one `wip: checkpoint ...` commit, push,
then resume each agent (same agent, same context) with a message naming the checkpoint commit and asking it to
re-check `git status` and re-run its tests before continuing. Done so for B2, B7, C1, C4 (commit 196d87d).
