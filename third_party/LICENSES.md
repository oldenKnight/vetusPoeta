# Third-party code, data and programs

vetus poeta itself is proprietary software (`LICENSE` in the repository root: all rights reserved; source visible
for review). This file lists everything in the repository, in the build, or in a distributed package that was made
by someone else, with its origin and licence. The BUILD task keeps this file current.

## Vendored source compiled into the programs

| File(s) | Project and version | Copyright | Licence |
|---|---|---|---|
| `third_party/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | 2013-2025 Niels Lohmann | MIT |
| `third_party/miniz.c`, `miniz.h` | [richgel999/miniz](https://github.com/richgel999/miniz) 3.0.2 | 2013-2014 RAD Game Tools and Valve Software; 2010-2014 Rich Geldreich and Tenacious Software LLC | MIT (text in `miniz.c`) |
| `third_party/doctest.h` (tests only, not shipped) | [doctest/doctest](https://github.com/doctest/doctest) 2.5.0 | 2016-2023 Viktor Kirilov | MIT |
| `third_party/llama/` (to be vendored by BUILD; pinned commit in `third_party/llama/VERSION`) | [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) | The ggml authors | MIT |

## Data shipped next to the program (separate files, their own licences; see the lexicon NOTE section and About)

| Data | Source | Licence |
|---|---|---|
| `latin.vpl`, `greek.vpl`, `english.vpl`, `spanish.vpl` (compiled lexicons) | Wiktionary via Kaikki/Wiktextract; Perseus Lewis & Short and LSJ; DCC Core Vocabulary; Whitaker's Words | CC BY-SA 4.0 (Wiktionary, Perseus), CC BY-SA 3.0 (DCC), Whitaker's permission text (quoted in NOTE) |
| `models/*.gguf` (optional download) | Qwen2.5-0.5B-Instruct Q4_K_M | Apache-2.0 |
| `gui/ui/fonts/GentiumPlus-*.ttf` | SIL Gentium Plus 6.101 | SIL Open Font License 1.1 (Reserved Font Names "Gentium", "SIL") |
