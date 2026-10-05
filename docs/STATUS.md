# STATUS — task ledger (resume from here after any crash)

Legend: TODO · WIP(agent) · REVIEW · DONE · BLOCKED. Only the main agent moves a task to DONE.
Subagents: do exactly one task, in your own files, then set REVIEW and commit **only your paths**
(`git add <your files>`; if `.git/index.lock` exists, wait 5 s and retry). Never `git add -A`.
Never push. Never spawn subagents.

## Milestones
- M0 skeleton + decisions + pre-plan (step 1 of the production process)
- M1 offline library built from Wiktionary (Latin, Greek, English pivot, Spanish)
- M2 engine core: lexicon, morphology, subtitle I/O, project format, JSON-lines server
- M3 rule engine EN/ES -> LA, acceptance loop on Alice, held-out measurement
- M4 UI + shell, i18n, logo, font
- M5 local model (engine ii) + online check (engine iii)
- M6 LA -> EN/ES, Greek, Orbergise
- M7 critic pass, Google Translate side-by-side, release packaging

## Tasks
| ID | Module | Task | Owner | State | Notes |
|---|---|---|---|---|---|
| M0.1 | docs | Repo skeleton: CLAUDE.md, DECISIONS.md, STATUS.md, HANDOFF.md, .gitignore | main | DONE | 2026-10-05 |
| M0.2 | data | Background download of Kaikki Latin, Ancient Greek, English and es.wiktionary extracts into data/raw (gitignored) | main | WIP(main) | logs in data/raw/dl-*.log, flag data/raw/dl-done.flag |
| M0.3 | docs | Sonnet pre-plan and pre-design: docs/PREPLAN.md, docs/PREDESIGN.md | sonnet | REVIEW | written 2026-10-05; numbers measured with throwaway scripts on data/raw (M1.1 must reproduce them); main agent redoes freely afterwards |
| M0.4 | docs | DESIGN.md v1 (contracts) written by main agent from the pre-work | main | TODO | |
