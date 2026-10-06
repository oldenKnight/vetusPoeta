#!/usr/bin/env bash
# Cross-compile the engine (no GUI) plus the vendored llama.cpp for Windows x64 with MinGW-w64, to catch
# portability errors early. Uses the `mingw-release` CMake preset (toolchain: cmake/mingw-w64.cmake).
#   tools/xcompile_check.sh            # build into build-mingw-release/
#   VP_WITH_LLM=OFF tools/xcompile_check.sh   # skip llama.cpp (faster)
# Prints `SKIPPED (no mingw)` and exits 0 when the cross compiler is not installed.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 && ! command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1; then
  echo "SKIPPED (no mingw)"
  exit 0
fi

BUILD_DIR=build-mingw-release
LLM="${VP_WITH_LLM:-ON}"
JOBS="$(nproc 2>/dev/null || echo 4)"
start=$(date +%s)

# The preset sets VP_BUILD_GUI=OFF and VP_BUILD_TESTS=OFF; the check turns tests back on because vp_tests (and, with
# the local model, vp_llama_smoke, which links llama + ggml statically) are the Windows executables that exist until
# the CLI module adds vpengine. Set VP_XC_TESTS=OFF to build only the libraries and engine executables.
cmake --preset mingw-release -DVP_WITH_LLM="$LLM" -DVP_BUILD_TESTS="${VP_XC_TESTS:-ON}" >/dev/null
if ! cmake --build "$BUILD_DIR" -j"$JOBS" >"$BUILD_DIR/xcompile.log" 2>&1; then
  tail -40 "$BUILD_DIR/xcompile.log"
  echo "xcompile FAILED (full log: $BUILD_DIR/xcompile.log)"
  exit 1
fi
if grep -E -q '(^|[^-])warning:' "$BUILD_DIR/xcompile.log"; then
  grep -E '(^|[^-])warning:' "$BUILD_DIR/xcompile.log" | head -20
  echo "xcompile: warnings above (our code must be -Wall -Wextra clean)"
fi

exes=$(find "$BUILD_DIR" -name '*.exe' -not -path '*/CMakeFiles/*' | sort)
libs=$(find "$BUILD_DIR" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
for f in $exes $libs; do
  printf '%10d  %s\n' "$(stat -c %s "$f")" "$f"
done
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
  for f in $exes; do
    dlls=$(x86_64-w64-mingw32-objdump -p "$f" | awk '/DLL Name:/ {print $3}' | sort -u | tr '\n' ' ')
    echo "  $(basename "$f") imports: $dlls"
    case "$dlls" in
      *libstdc++*|*libgcc_s*|*libwinpthread*) echo "xcompile FAILED: $f depends on a MinGW runtime DLL"; exit 1;;
    esac
  done
fi
echo "xcompile OK ($(( $(date +%s) - start )) s)"
