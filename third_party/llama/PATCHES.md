# Local patches to the vendored llama.cpp tree

None. The tree under `third_party/llama/` is byte-identical to upstream at the commit in `VERSION`
(only a subset of files is copied). Build integration lives in `cmake/vp_llama.cmake`, which adds `ggml/` and
`src/` directly instead of the upstream top-level `CMakeLists.txt` (that one adds `vendor/`, which is not vendored).

Build-side workaround (not a source patch): MinGW-w64 v11 headers (Ubuntu 24.04 `g++-mingw-w64`) lack
`THREAD_POWER_THROTTLING_STATE`, used by `ggml/src/ggml-cpu/ggml-cpu.c` (`ggml_thread_apply_priority`) when
`_WIN32_WINNT >= 0x0602`. `cmake/vp_llama.cmake` probes the toolchain and, only if the type is missing, force-includes
`cmake/compat/mingw_thread_power.h` into that single file. MSVC and newer MinGW headers are unaffected.

Cosmetic: ggml's own CMake asks git for the commit of the directory it sits in, so `GGML_COMMIT` in the generated
`ggml-version.h` shows the vetus poeta commit, not the upstream one. `LLAMA_COMMIT` is set from `VERSION`.

To update: clone upstream at the new tag, copy the same subset (list in `VERSION`), update `VERSION` and
`third_party/LICENSES.md`, rebuild on Linux, run `tools/xcompile_check.sh`, and record any patch here
(file, reason, diff summary).
