# Building vetus poeta

The engine is C++17 and builds with CMake >= 3.20 (presets need >= 3.21). Everything it links is vendored in
`third_party/` (see `third_party/LICENSES.md`); nothing is downloaded at build time, except the WebView2 SDK for
the Windows shell (below; it has an offline fallback).

| Option | Default | Meaning |
|---|---|---|
| `VP_BUILD_GUI` | ON on Windows, OFF elsewhere | WebView2 shell (`gui/shell`), Windows only |
| `VP_BUILD_TESTS` | ON | doctest unit tests (`vp_tests`) and, with the model, `vp_llama_smoke` |
| `VP_SANITIZE` | OFF | ASan + LSan + UBSan (GCC/Clang) |
| `VP_WITH_LLM` | ON | local model engine: builds vendored llama.cpp (CPU only) as target `vp_llama` |

## Linux (GCC 13 or Clang)

```sh
cmake -S . -B build -DVP_BUILD_GUI=OFF
cmake --build build -j4
ctest --test-dir build --output-on-failure
```
or with presets: `cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release`
(build directory `build-linux-release/`).

Without the local model (much faster; llama.cpp is not compiled at all): add `-DVP_WITH_LLM=OFF`.

Reference times on 4 cores: clean configure + build with the model about 60 s (llama.cpp is most of it), without
the model about 5 s at the current module count.

## Sanitizer build (part of every task's definition of done)

```sh
cmake --preset linux-asan            # Debug, VP_SANITIZE=ON, build-linux-asan/
cmake --build --preset linux-asan
ctest --preset linux-asan            # sets ASAN_OPTIONS=detect_leaks=1 and halt-on-error UBSan
```
The sanitizer flags apply to the vendored llama.cpp too, so this build takes about 4.5 min on 4 cores with the
model on. Add `-DVP_WITH_LLM=OFF` to the configure line for a quick module-only sanitizer run. Zero findings
(errors and leaks) is required.

## Windows cross build with MinGW-w64 (from Linux)

```sh
sudo apt install g++-mingw-w64-x86-64      # provides x86_64-w64-mingw32-g++(-posix)
tools/xcompile_check.sh                    # builds build-mingw-release/ and lists the .exe files
VP_BUILD_GUI=ON tools/xcompile_check.sh    # the same plus the shell VetusPoeta.exe
```
The script uses the `mingw-release` preset (toolchain `cmake/mingw-w64.cmake`, GUI off unless `VP_BUILD_GUI=ON`)
with tests turned on, so `vp_tests.exe` and `vp_llama_smoke.exe` are cross-linked; it fails if any `.exe` imports a
MinGW runtime DLL (everything is linked with `-static`). `VP_WITH_LLM=OFF tools/xcompile_check.sh` skips llama.cpp.
If no MinGW compiler is installed it prints `SKIPPED (no mingw)` and exits 0. Running the executables needs Windows
(or Wine); the script only compiles and links.

The shell alone, then the portable folder:
```sh
cmake --preset mingw-release -DVP_BUILD_GUI=ON -DVP_WITH_LLM=OFF
cmake --build build-mingw-release -j4      # VetusPoeta.exe, vpengine.exe; build-mingw-release/app/ is runnable
python3 tools/make_dist.py --mingw build-mingw-release     # -> dist/vetus-poeta/ (add --no-data to skip lexicons)
```
MinGW cannot link Microsoft's static WebView2 loader library, so the MinGW shell loads `WebView2Loader.dll` at run
time and that DLL ships next to the exe. Reference sizes (MinGW, Release, static): `VetusPoeta.exe` 1.6 MB,
importing only Windows system DLLs (KERNEL32, USER32, GDI32, ADVAPI32, SHELL32, ole32, dwmapi, msvcrt);
`vpengine.exe` 4.3 MB without the model.

MinGW-w64 v11 headers (Ubuntu 24.04) lack `THREAD_POWER_THROTTLING_STATE`, which ggml uses; `cmake/vp_llama.cmake`
probes for it and, only when missing, force-includes `cmake/compat/mingw_thread_power.h` into `ggml-cpu.c`.
No vendored file is edited (`third_party/llama/PATCHES.md`).

## Windows with MSVC 2022 (native)

Needs Windows 10/11 x64, Visual Studio 2022 with "Desktop development with C++" (compiler, Windows SDK, CMake),
and Python 3 on `PATH` (the build copies the UI with `tools/pack_ui.py`). From an "x64 Native Tools Command
Prompt for VS 2022":
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tools\make_dist.py --msvc build --config Release
```
* `VP_BUILD_GUI` defaults to ON on Windows. Use `-DVP_BUILD_GUI=OFF` for an engine-only build.
* The static CRT is used (`/MT`, `CMAKE_MSVC_RUNTIME_LIBRARY`), `/W4 /utf-8 /EHsc`; vendored llama.cpp is compiled
  with `/w` and its headers are treated as external (SYSTEM) includes.
* llama.cpp is built with `GGML_NATIVE=OFF` and AVX2 + FMA + F16C + BMI2 so the binary does not depend on the build
  machine; the engine checks CPUID at model load and reports `model_unsupported_cpu` on a CPU without AVX2.
* `build\app\Release\` is a runnable folder (VetusPoeta.exe, vpengine.exe, ui\); start the app from there or
  from `dist\vetus-poeta\`, never from `build\gui\shell\Release\` (no engine and no ui next to it there).
  `build\app\` has no `data\` folder: set `VP_LEXICON_DIR` to the folder with the `.vpl` files (e.g.
  `data\work`) or use the dist folder, which has them.
* The MSVC build of this tree has not yet been verified on a Windows machine.

### The WebView2 SDK
At configure time `gui/shell/CMakeLists.txt` downloads the **Microsoft.Web.WebView2** NuGet package 1.0.2903.40
from nuget.org into `<build>/_deps/webview2-1.0.2903.40/` (once per build folder) and uses its `WebView2.h`. MSVC
links `WebView2LoaderStatic.lib` (plus `version.lib`), so no loader DLL ships; MinGW uses `WebView2Loader.dll`.
Offline: download `https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40` elsewhere, unzip it
(it is a zip) and pass `-DVP_WEBVIEW2_SDK_DIR=<folder>`; or `-DVP_WEBVIEW2_DOWNLOAD=OFF`, which compiles against
the checked-in `gui/shell/webview2_subset.h` (same declarations, `gui/shell/tools/webview2_subset.py verify`)
and the dynamic loader: then copy an x64 `WebView2Loader.dll` next to `VetusPoeta.exe` yourself.

### The WebView2 Runtime (end users)
The window is drawn by the Microsoft Edge WebView2 Runtime (Evergreen), which Windows 10 (updated) and Windows 11
already have. If it is missing, VetusPoeta.exe says so and offers to open Microsoft's download page; install the
Evergreen Bootstrapper or the Standalone Installer from https://developer.microsoft.com/microsoft-edge/webview2/
(the standalone x64 installer works without network on the target machine). The app needs a runtime that supports
`SetVirtualHostNameToFolderMapping` and `postMessageWithAdditionalObjects` (any Evergreen runtime from 2023 on).

## Windows: the dist folder

`tools/make_dist.py` writes `dist/vetus-poeta/` (portable: copy it anywhere, no installer, no VC++ redistributable):
```
VetusPoeta.exe          the window (gui/shell)            vpengine.exe        the translator (engine/cli)
WebView2Loader.dll      MinGW builds only                 ui/ + ui.manifest.json   gui/ui without dev/ and tests/,
data/*.vpl              lexicons from data/work/                                   size + SHA-256 per file
models/README.txt       where the optional model goes     licenses/           WebView2 SDK licence
THIRD_PARTY_NOTICES.txt third_party/LICENSES.md, licence texts, the NOTE section of every lexicon
```
Per-user files live in `%LOCALAPPDATA%\vetus-poeta\` (or `%VP_DATA_DIR%`): `settings.json`, `window.json`
(size, position, maximised), `unsaved\` autosaves, `logs\engine.log` (engine and shell log, rotated at 2 MB),
`WebView2\` (browser profile; deleting it is safe). `VetusPoeta.exe --devtools` turns on the WebView2 DevTools
(F12), the context menu and the browser shortcuts; release runs have them off. A file given on the command line
(`.vpoeta .srt .vtt .ass .ssa .txt`) opens at start; a second launch hands its file to the running window.

### Owner smoke test on Windows (the shell was only cross-compiled here)
1. Start `dist\vetus-poeta\VetusPoeta.exe`: window titled "vetus poeta", app icon in title bar and taskbar, the
   UI appears; no console window; Task Manager shows `vpengine.exe` under it.
2. Resize to the minimum (1024x640 client at 100 %); at 125 % and 150 % scaling the page fills the window exactly
   (no offset under the title bar, nothing clipped); drag between monitors with different scaling.
3. Close, reopen: same size and position; maximised stays maximised (`window.json`).
4. Settings > theme Dark / Light / Auto: the title bar follows; Auto follows the Windows app mode.
5. Choose file... (dialog.openFile) with filters; Save as (dialog.saveFile) suggests the name; cancel both.
6. Drag a `.srt` from Explorer onto the start screen and onto the workspace: it opens (real path).
7. With the app open, double-click / run `VetusPoeta.exe C:\path\film.srt`: no second window, the first one comes
   to the front and opens the file.
8. End `vpengine.exe` in Task Manager while a project is open: within ~2 s the toast "The translator restarted.
   Your work is safe." and the project is back (`engine.restarted`). Kill it 4 times within a minute: a native
   message with the log path, no further restarts. Suspend it (Resource Monitor > Suspend process): restart
   after about 10 s.
9. Close the app: `vpengine.exe` exits (also when VetusPoeta.exe is killed: job object), no lock file is left.
10. Unplug the charger on a laptop: the UI gets `power.status {onBattery:true}`.
11. `--devtools`: F12 opens DevTools; without it, F12, Ctrl+R, Ctrl+P, right-click menu and Ctrl+wheel zoom do
    nothing; no network traffic while idle (Resource Monitor > Network, filter msedgewebview2.exe and vpengine.exe).
12. Rename `ui\` or `vpengine.exe`, or remove `WebView2Loader.dll`: a clear native message, no crash.

## The local model file

* Engine ii uses Qwen2.5-0.5B-Instruct Q4_K_M (Apache-2.0) in GGUF format. It is optional: without it the rule
  engine works and the model features report "unavailable".
* Put the file in the `models/` directory next to the engine (development: `models/` at the repository root).
  Its expected SHA-256 is recorded in `models/README.md`; the engine verifies it and reports the result as
  `sha256ok` in the model status (DESIGN.md section 11). Check by hand with `sha256sum models/*.gguf`.
* `*.gguf` files are never committed (`.gitignore`); the installer offers the download.

## Tests

* C++: `ctest --test-dir <build-dir> --output-on-failure` runs `vp_tests` (all modules, doctest; pass
  `--test-case=<pattern>` to the executable to run a subset) and `vp_llama_smoke` (initialises the llama.cpp CPU
  backend without a model, no network).
* JavaScript: `tools/jstest.sh` (Node >= 18, no npm packages).
* Library pipeline: `python3 tools/build_library/build.py --check`.

## Vendored llama.cpp

`third_party/llama/` is a CPU-only subset of upstream pinned in `third_party/llama/VERSION` (commit, tag, date,
URL; about 10 MB). `cmake/vp_llama.cmake` adds its `ggml/` and `src/` directories directly (the upstream top-level
CMakeLists.txt is not used), forces the options (no GPU, no OpenMP, no native tuning, static, no tools/tests), compiles
it with warnings off and exposes the INTERFACE target `vp_llama` (links `llama` + `ggml`, include directories as
SYSTEM, defines `VP_WITH_LLM=1`). Update procedure: `third_party/llama/PATCHES.md`.
