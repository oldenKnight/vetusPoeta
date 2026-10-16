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
| `third_party/llama/` (CPU-only subset; `VERSION`, `PATCHES.md`: no local patches) | [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) tag b11433, commit `50569eb87df530daff11afda229ceb9ab8e6cae8` (2026-10-06) | 2023-2026 The ggml authors | MIT (text in `third_party/llama/LICENSE`) |

## Data shipped next to the program (separate files, their own licences; see the lexicon NOTE section and About)

| Data | Source | Licence |
|---|---|---|
| `latin.vpl`, `greek.vpl`, `english.vpl`, `spanish.vpl` (compiled lexicons) | Wiktionary via Kaikki/Wiktextract; Perseus Lewis & Short and LSJ; DCC Core Vocabulary; Whitaker's Words | CC BY-SA 4.0 (Wiktionary, Perseus), CC BY-SA 3.0 (DCC), Whitaker's permission text (quoted in NOTE) |
| `models/*.gguf` (optional download) | Qwen2.5-0.5B-Instruct Q4_K_M | Apache-2.0 |
| `gui/ui/fonts/GentiumPlus-*.ttf` | SIL Gentium Plus 6.101 | SIL Open Font License 1.1 (Reserved Font Names "Gentium", "SIL") |

## Windows shell: Microsoft Edge WebView2

| What | Where | Origin | Licence |
|---|---|---|---|
| `WebView2.h`, `WebView2LoaderStatic.lib` (MSVC, linked into `VetusPoeta.exe`), `WebView2Loader.dll` (MinGW, shipped next to the exe) | downloaded at configure time into `<build>/_deps/webview2-1.0.2903.40/`, never committed | NuGet package [Microsoft.Web.WebView2](https://www.nuget.org/packages/Microsoft.Web.WebView2) 1.0.2903.40, Microsoft Corporation | WebView2 SDK licence (BSD-style, `LICENSE.txt` in the package; `tools/make_dist.py` ships it as `licenses/WebView2-LICENSE.txt` and in `THIRD_PARTY_NOTICES.txt`) |
| `gui/shell/webview2_subset.h` | committed | declarations copied from the same package's `WebView2.h` by `gui/shell/tools/webview2_subset.py` (see below) | WebView2 SDK licence (notice in the file header) |
| Microsoft Edge WebView2 Runtime (Evergreen) | part of Windows 10/11, not shipped | Microsoft Corporation | Microsoft software licence terms of the runtime |

### Provenance of `gui/shell/webview2_subset.h`
The shell normally compiles against the genuine `WebView2.h` that CMake downloads (`gui/shell/CMakeLists.txt`).
`webview2_subset.h` is only the fallback when that download is impossible (`-DVP_WEBVIEW2_DOWNLOAD=OFF` or no
network); it is then used with the dynamic loader. It was generated on 2026-10-06 by
`gui/shell/tools/webview2_subset.py gen` (a port of the prototype's own script, same owner, proprietary like the
project) from the MIDL-generated `WebView2.h` of Microsoft.Web.WebView2 **1.0.2903.40** downloaded from
`https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40`. It holds, unchanged, the IIDs and
vtable-ordered method declarations of the 28 interfaces the shell uses, the 7 enums/structs they name, and
forward declarations of the other interface names in those signatures. `webview2_subset.py verify <WebView2.h>`
checks IID, base and method order of every interface against a genuine header (28/28 ok on generation).
