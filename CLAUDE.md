# vetus poeta — rules for every Claude session (main agent and subagents)

Read this first, then `docs/STATUS.md` (the ledger), then `docs/HANDOFF.md`, then the part of
`docs/DESIGN.md` for your module. Nothing in chat history is authoritative; these files are.
If the session crashed or was stopped, resume from `docs/STATUS.md`. Never redo a task marked
DONE. Never re-derive context that the ledger already holds.

## What this is
An offline Latin and Ancient Greek translator for a teacher and beginner-intermediate students.
Windows 10/11 desktop app: C++17 engine as a sidecar process, WebView2 shell, ES5 vanilla JS UI.
Prototype for architecture and conventions: the BabyDaVinci repo (same owner). Full product
decisions are in `docs/DECISIONS.md`.

## Roles
- Main agent (planner / designer / reviewer / harsh critic) owns `docs/DESIGN.md`, `docs/STATUS.md`,
  `docs/DECISIONS.md`, reviews, and is the only one who marks a task DONE.
- Implementer subagents (Opus) implement exactly one task from `docs/STATUS.md`, inside the
  directories that task names. They do not edit other modules' directories and do not change
  public headers under `engine/*/include/` or the engine protocol without recording the reason
  in `docs/STATUS.md` ("API changes").
- Subagents never spawn subagents, never push, never `git add -A` (add only your own paths).

## Definition of done (every task)
1. Builds on Linux: `cmake -S . -B build -DVP_BUILD_GUI=OFF && cmake --build build -j4`.
2. Tests pass: `ctest --test-dir build --output-on-failure`. JS has its own tests (`tools/jstest.sh`).
3. Engine cross-compiles for Windows: `tools/xcompile_check.sh` (MinGW, engine only).
4. No warnings on `-Wall -Wextra` in your module. No exceptions escape a module boundary.
5. Sanitizer build clean (ASan + LSan + UBSan): zero leaks, zero errors.
6. `docs/STATUS.md` row set to REVIEW with notes, then one commit: `<module>: <what>`.

## Hard rules
- Offline means offline. Engines i (rules) and ii (local model) and the offline library must
  work with the network unplugged. Only the online check and the online library may touch the
  network, and only when the user toggled them on. Any other network call is a bug.
- C++17, no external binaries linked except vendored libs in `third_party/` and, on Windows,
  WebView2 (shell only). The local model runs through vendored llama.cpp (CPU only).
- JavaScript is ES5 only: `var`, `function`, no arrow functions, no `let`/`const`, no classes,
  no template strings, no Promise unless polyfilled. Separate files. Every file is one IIFE that
  exposes exactly one global namespaced `VP_<Name>` (collision check before naming anything).
- User-facing strings live in `gui/ui/i18n/en-US.json` and `gui/ui/i18n/es-MX.json`, never inline.
- Deterministic: same input + same settings = byte-identical output (engine i). Engine ii uses a
  fixed seed and greedy or constrained decoding.
- Subtitle files: timing and cue numbering are copied byte for byte; only text lines change.
  Styling tags (`<i>`, `{\an8}`, ASS override blocks) are preserved.
- Never crash: every engine command catches at the boundary and returns an error with a
  human `hint`. The shell supervises the engine and restarts it with `project.recover`.
- No model identifiers or session links in code, comments, commits or docs.
- Stuck for more than 15 minutes on one problem: write what you tried in `docs/STATUS.md` under
  the task as BLOCKED and stop. Do not loop.
- Do not paste copyrighted text (textbook pages, film scripts, subtitle files) into the repo.
  Vocabulary lists and frequency data are fine; sample files must be our own sentences.

## Memory discipline (hard rules, enforced by sanitizer runs)
- RAII only: no raw `new`/`delete`, no `malloc`/`free` outside vendored C libs. OS handles live in
  RAII wrappers; every early return releases them.
- The lexicon is a memory-mapped read-only file with offset tables; never load it into heap
  structures. Caches have an explicit cap and an eviction rule. No unbounded queues.
- The local model is loaded on demand, unloaded after a job (or after an idle timeout), never
  resident while the user edits.
- Threads are joined. No detached threads.
- Long jobs are checked for growth: RSS after 1000 cues must not exceed RSS after 10 cues by
  more than 5 %.
- JS side: remove listeners you add, cap caches, null out large arrays when done, never keep
  more than the visible window of cues in DOM.
