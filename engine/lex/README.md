# engine/lex (`vp_lex`)

Read-only reader of the `.vpl` lexicon files (`latin.vpl`, `greek.vpl`, `english.vpl`, `spanish.vpl`).
Contract: DESIGN.md §5 (layout) and §5.1 (API). Static library; links `vp_core` (`vp::MappedFile`, `vp::Sha256`).
Public header: `include/vp/lex.h`; feature packing: `include/vp/features.h` (contract, shared with
`tools/build_library/features.py`).

## File layout (little-endian, offsets from file start, sections 8-byte aligned)
```
header 256 B   "VPLX" | u16 major=1 @4 | u16 minor @6 | char[8] lang @8 | u32 section_count @16
               | u64 file_size @20 | u8[32] sha256 of [256, file_size) @28 | zeros
table @256     section_count x {char[4] tag, u32 reserved, u64 offset, u64 length}
NOTE STRS KEYS ANAL LEMM SENS FEAT GENX REVX   (en/es: NOTE STRS KEYS ANAL LEMM FEAT)
```
Record sizes: ANAL 12, LEMM 48, SENS 16, FEAT 4, GENX 8, CAND 8. Exact offsets of the committed fixtures:
`tests/fixtures/lex/SPEC_CHECK.md`.

## API (`vp::lex::Lexicon`)
- `open(path)` maps the file and validates: magic, major == 1 (`lexicon_version` otherwise), header file_size ==
  real size, every section inside the file, after the table and 8-byte aligned, no duplicate tags, NOTE STRS KEYS
  ANAL LEMM FEAT present, every count x record size inside its section, STRS ending in NUL. Missing file:
  `lexicon_missing`; anything else: `lexicon_corrupt`. Unknown section tags are skipped.
- `verifySha256()` hashes the body (touches every page; call it from `inspect`/diagnostics, not on startup).
- `lookup` (binary search on KEYS, bytewise), `prefix`, `lemma`, `senses`, `generate` (binary search on the
  lemma's GENX cells by feat_id), `cells`, `reverse` (binary search on REVX keywords; candidates returned score
  desc, lemma id asc, sense asc), `feature` / `featId`, `stats()`, `lang()`, `notice()`.
- All `string_view`s point into the map and die with the Lexicon. Queries append to the caller's vectors and
  allocate nothing else. Every offset read from the file is bounds-checked when used: a damaged record gives an
  empty view or `false`, never a read outside the map. `lemma(id)` with a bad id returns `id == kNoLemma`.
- `emoji_off == 0` means no emoji. Movable, not copyable; a moved-from Lexicon answers nothing.

## Fixtures and the reference encoder
`engine/tests/lex_fixture_writer.{h,cpp}` (test code, not shipped) encodes `.vpl` bytes from plain structs exactly
per §5, every field offset commented. It is the reference for `tools/build_library/pack.py` until that exists.
Its STRS convention (NUL at 0, then distinct strings sorted bytewise) lets the packer match bytes exactly.
Committed fixtures in `tests/fixtures/lex/`:
- `latin.vpl`: puella (13 cells incl. archaic gen. -āī), amō (42 cells + a form-of analysis), bonus (36),
  virgō, dīligō, amor; two senses each for puella and amō; REVX for girl, love and seven more keywords.
- `greek.vpl`: ἄνθρωπος, Attic paradigm (extra Attic bit) plus Epic/Ionic forms with ANAL flag non-Attic.
- `english.vpl`: morphology only (go, see, saw, child).
The test "lex: committed fixtures match the reference encoder" fails when the encoder and the files drift.
Regenerate files and SPEC_CHECK.md with:
```
VP_REGEN_LEX_FIXTURES=1 ./build/engine/tests/vp_tests -tc='lex: committed fixtures*'
```

## Tests
`engine/tests/test_lex.cpp` (doctest, in `vp_tests`): lookups, prefix, generate, reverse ordering, features
golden file, open errors, fuzz (200 truncations, 500 byte flips, zero-filled ranges, boundary u32 values; run it
under `-DVP_SANITIZE=ON`), RSS-flat over 1,000,000 lookups on a generated 400,000-key file, lookups per second.
```
cmake -S . -B build -DVP_BUILD_GUI=OFF -DVP_WITH_LLM=OFF && cmake --build build -j4
./build/engine/tests/vp_tests -tc='lex*' -s | grep MESSAGE
```
