# Building vetus poeta

The engine is C++17 and builds with CMake >= 3.20 (presets need >= 3.21). Everything it links is vendored in
`third_party/` (see `third_party/LICENSES.md`); nothing is downloaded at build time.

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
```
The script uses the `mingw-release` preset (toolchain `cmake/mingw-w64.cmake`, GUI off) with tests turned on, so
`vp_tests.exe` and `vp_llama_smoke.exe` are cross-linked; it fails if any `.exe` imports a MinGW runtime DLL
(everything is linked with `-static`). `VP_WITH_LLM=OFF tools/xcompile_check.sh` skips llama.cpp. If no MinGW
compiler is installed it prints `SKIPPED (no mingw)` and exits 0. Running the executables needs Windows (or Wine);
the script only compiles and links.

MinGW-w64 v11 headers (Ubuntu 24.04) lack `THREAD_POWER_THROTTLING_STATE`, which ggml uses; `cmake/vp_llama.cmake`
probes for it and, only when missing, force-includes `cmake/compat/mingw_thread_power.h` into `ggml-cpu.c`.
No vendored file is edited (`third_party/llama/PATCHES.md`).

## Windows with MSVC 2022 (native)

From an "x64 Native Tools Command Prompt for VS 2022":
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
* `VP_BUILD_GUI` defaults to ON on Windows and needs the WebView2 SDK (see `gui/shell/README.md` once the shell
  exists). Use `-DVP_BUILD_GUI=OFF` for an engine-only build.
* The static CRT is used (`/MT`, `CMAKE_MSVC_RUNTIME_LIBRARY`), `/W4 /utf-8 /EHsc`; vendored llama.cpp is compiled
  with `/w` and its headers are treated as external (SYSTEM) includes.
* llama.cpp is built with `GGML_NATIVE=OFF` and AVX2 + FMA + F16C + BMI2 so the binary does not depend on the build
  machine; the engine checks CPUID at model load and reports `model_unsupported_cpu` on a CPU without AVX2.
* The MSVC build of this tree has not yet been verified on a Windows machine.

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
