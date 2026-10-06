# Held-out set (DESIGN §15, PREPLAN 6.5)

Frozen before any rule work. Implementers never open these files; only `tools/eval` reads them, and only to
produce numbers. Any cue inspected for debugging must be moved to `tests/regression/` and listed in `BURNED.txt`.

| File | Origin | Cues |
|---|---|---|
| `oz_dialogue.en.srt` | dialogue sentences of L. Frank Baum, *The Wonderful Wizard of Oz* (1900, public domain; Project Gutenberg #55), extracted by `tools/eval/make_heldout.py`, synthetic timing | see FROZEN.sha256 |
| `own_heldout.en.srt` | sentences written for this project (`own_heldout.en.txt`), synthetic timing | |

The owner's second subtitle file replaces these as the primary held-out set when it arrives (DECISIONS D7).
`FROZEN.sha256` holds the SHA-256 of every file here; `tools/eval` refuses to report held-out numbers if a hash differs.
