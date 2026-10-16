# Cross-compile for 64-bit Windows with MinGW-w64 (Debian/Ubuntu package g++-mingw-w64-x86-64).
# Used by the `mingw-release` preset and tools/xcompile_check.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(VP_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 triplet prefix")
# Prefer the -posix flavour (std::thread / std::mutex via winpthreads) when installed; the -win32 flavour of
# GCC 13 also has them, so fall back to the plain name.
find_program(VP_MINGW_CC  NAMES ${VP_MINGW_PREFIX}-gcc-posix ${VP_MINGW_PREFIX}-gcc REQUIRED)
find_program(VP_MINGW_CXX NAMES ${VP_MINGW_PREFIX}-g++-posix ${VP_MINGW_PREFIX}-g++ REQUIRED)
find_program(VP_MINGW_RC  NAMES ${VP_MINGW_PREFIX}-windres)
set(CMAKE_C_COMPILER   ${VP_MINGW_CC})
set(CMAKE_CXX_COMPILER ${VP_MINGW_CXX})
if(VP_MINGW_RC)
  set(CMAKE_RC_COMPILER ${VP_MINGW_RC})
endif()

set(CMAKE_FIND_ROOT_PATH /usr/${VP_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
