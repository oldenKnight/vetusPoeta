# engine/subs — subtitle I/O (target `vp_subs`)

Reads and writes `.srt`, `.vtt`, `.ass`/`.ssa` and `.txt` so that numbering, timing, styling tags and every
byte of layout survive translation. Only text changes. Contract: DESIGN.md §7; API: `include/vp/subs.h`.
Depends on the standard library and the header-only `vp/result.h`; no link dependency on `vp_core`.

## API in one screen
- `parse(bytes, hint)` -> `Document` (never fails on malformed input; problems go to `Document::warnings`).
- `write(doc, options, &warnings)` -> bytes in the original encoding/BOM unless `WriteOptions` overrides.
- `Cue::spans`: `Text`, `Tag` (opaque: `<i>`, `<v Name>`, `<c.x>`, `<ruby>`, VTT timestamps, `{\an8}`, ASS
  `{...}`), `Newline` (exact break: `\n`, `\r\n`, or ASS `\N`/`\n`). `Cue::plainText()` for checks and cps.
- `breakLines(text, hints, 42, 2, &overflow)`, `charsPerSecond(cue)`, `parseTiming(...)`, `visibleLength(...)`,
  `splitSpans(text, format)` (for rules building new text).

## How the byte-exact round trip works
The decoded text is split into lines with their own line breaks. Everything that is not cue text is stored raw:
`headerRaw` (before the first cue), per cue `idRaw`+`idEol`, `timingRaw`+`timingEol`, `textEol`, `tailRaw`
(blank lines and anything else up to the next cue) and `trailerRaw` (after the last cue). `write` concatenates
them back. A cue whose spans still equal `parsedText` is written verbatim; a changed cue is re-broken
(`rebreak=true`, never for TXT) and joined with the cue's format break.

Format notes:
- SRT/VTT: a cue starts at a line with `-->` whose left side is a timestamp; the line above is its identifier when
  it is a number or follows a blank line. VTT cue settings stay inside `timingRaw`. NOTE/STYLE/REGION blocks
  before the first cue are in `headerRaw`; NOTE blocks between cues are in the previous cue's `tailRaw`.
- ASS/SSA: fields are read from the `[Events]` `Format:` line. `styleRaw` is the Dialogue line up to the Text
  field; `timingRaw` is a copy of `Start,End` for reading speed (the writer writes `styleRaw`). `Comment:` lines,
  blank lines and other sections stay verbatim in `headerRaw`/`tailRaw`/`trailerRaw` (they are never cues).
- TXT: one cue per paragraph (blank-line separated), no timing.
- Encodings: UTF-8 (BOM or not), UTF-16 LE/BE with BOM, otherwise Windows-1252 (the five undefined bytes map to
  the C1 controls of the same value, so every byte sequence round-trips). Content signatures (`WEBVTT`,
  `[Script Info]`) override a wrong hint with a `format_mismatch` warning.

## Round-trip exceptions (documented, tested)
- UTF-16 input with an unpaired surrogate or an odd byte count: repaired (U+FFFD / byte dropped) and reported as
  `encoding_lossy`. Every other input, well-formed or not, round-trips byte for byte (the fuzz test checks it on
  500 mutated files).
- Malformed lines that are not recognised as timing (for example `->` instead of `-->`) are kept verbatim but
  belong to the previous cue's text or tail, so they are not translated as separate cues.

## Line breaking
Code points count, combining marks and tags do not. Words, tags and `{...}` blocks are never split. Cost =
sum of squared line widths (balance) minus bonuses for breaks after `.!?` (120) / `,;:` (80) and before hint
words (60), minus penalties for ending a line on a hint word (60) or a word of <= 3 letters (80). A dialogue
turn (`- `, `–`, `—` after a sentence end) starts its own line whenever maxLines >= 2. The fewest lines that
fit win; when none fit, the most even overflow is returned and `overflow` is set.

## Tests
`engine/tests/test_subs.cpp` with fixtures in `tests/fixtures/subs/` (own sentences only):
`ctest --test-dir build --output-on-failure` or `build/engine/tests/vp_tests -tc="subs:*"`.
