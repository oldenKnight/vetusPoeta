# Builds the vendored llama.cpp tree (third_party/llama, pinned in third_party/llama/VERSION) as static, CPU-only
# libraries and exposes one INTERFACE target `vp_llama`. Included from the top-level CMakeLists.txt when VP_WITH_LLM.
#
# The upstream top-level llama.cpp CMakeLists.txt is NOT used: it adds vendor/, common/, tools/ and install rules
# that we do not ship. Instead we add ggml/ and src/ directly and provide the few variables and the one function
# (llama_add_compile_flags) that src/CMakeLists.txt expects from the upstream top level. No vendored file is edited.

set(VP_LLAMA_DIR ${CMAKE_SOURCE_DIR}/third_party/llama)
file(STRINGS ${VP_LLAMA_DIR}/VERSION _vp_llama_commit REGEX "^commit=")
string(REPLACE "commit=" "" _vp_llama_commit "${_vp_llama_commit}")
string(SUBSTRING "${_vp_llama_commit}" 0 7 _vp_llama_short)

# Options (PREPLAN 2.3). CACHE FORCE so a stale cache or a parent cannot turn on GPU, OpenMP or native tuning.
function(vp_llama_force name value)
  set(${name} ${value} CACHE BOOL "forced by vetus poeta (cmake/vp_llama.cmake)" FORCE)
  mark_as_advanced(${name})
endfunction()
foreach(_opt GGML_NATIVE GGML_OPENMP GGML_CCACHE GGML_ALL_WARNINGS GGML_FATAL_WARNINGS GGML_BACKEND_DL
             GGML_CPU_ALL_VARIANTS GGML_BUILD_TESTS GGML_BUILD_EXAMPLES GGML_CUDA GGML_VULKAN GGML_METAL GGML_BLAS
             GGML_RPC GGML_SYCL GGML_OPENCL GGML_AVX512 GGML_AVX_VNNI GGML_CPU_KLEIDIAI
             LLAMA_BUILD_COMMON LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER
             LLAMA_BUILD_APP LLAMA_CURL LLAMA_OPENSSL LLAMA_ALL_WARNINGS)
  vp_llama_force(${_opt} OFF)
endforeach()
foreach(_opt GGML_CPU GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C GGML_BMI2 GGML_LLAMAFILE GGML_CPU_REPACK)
  vp_llama_force(${_opt} ON)
endforeach()

# Static libraries only, without changing BUILD_SHARED_LIBS for the rest of the project.
set(_vp_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)

# Variables the upstream top level normally provides to ggml/ and src/.
set(LLAMA_VERSION_MAJOR 0)
set(LLAMA_VERSION_MINOR 6)
set(LLAMA_VERSION_PATCH 0)
set(LLAMA_VERSION_BASE "${LLAMA_VERSION_MAJOR}.${LLAMA_VERSION_MINOR}.${LLAMA_VERSION_PATCH}")
set(LLAMA_VERSION "${LLAMA_VERSION_BASE}")
set(LLAMA_BUILD_NUMBER 0)
set(LLAMA_BUILD_COMMIT "${_vp_llama_short}")
set(GGML_BUILD_NUMBER ${LLAMA_BUILD_NUMBER})
function(llama_add_compile_flags)  # upstream adds its warning set here; vendored code is compiled with -w instead
endfunction()

add_subdirectory(${VP_LLAMA_DIR}/ggml ${CMAKE_BINARY_DIR}/third_party/llama/ggml)
add_subdirectory(${VP_LLAMA_DIR}/src  ${CMAKE_BINARY_DIR}/third_party/llama/src)
set(BUILD_SHARED_LIBS ${_vp_saved_shared})

# Our -Wall -Wextra -Wshadow (and /W4) must not apply to third-party code.
foreach(_t llama ggml ggml-base ggml-cpu)
  if(TARGET ${_t})
    if(MSVC)
      target_compile_options(${_t} PRIVATE /w)
    else()
      target_compile_options(${_t} PRIVATE -w)
    endif()
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
      set_target_properties(${_t} PROPERTIES SYSTEM ON)
    endif()
  endif()
endforeach()

# MinGW-w64 v11 headers (Ubuntu 24.04) lack THREAD_POWER_THROTTLING_STATE, used by ggml-cpu.c when
# _WIN32_WINNT >= 0x0602. Probe the toolchain and, only if missing, force-include a definition into that one file.
# Lowering _WIN32_WINNT instead would also switch off PrefetchVirtualMemory in llama-mmap.cpp.
if(MINGW AND TARGET ggml-cpu)
  include(CheckCSourceCompiles)
  check_c_source_compiles("
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    int main(void) { THREAD_POWER_THROTTLING_STATE t; t.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION; return (int)t.Version; }"
    VP_MINGW_HAS_THREAD_POWER_THROTTLING)
  if(NOT VP_MINGW_HAS_THREAD_POWER_THROTTLING)
    set_source_files_properties(${VP_LLAMA_DIR}/ggml/src/ggml-cpu/ggml-cpu.c TARGET_DIRECTORY ggml-cpu PROPERTIES
      COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/cmake/compat/mingw_thread_power.h")
  endif()
endif()

add_library(vp_llama INTERFACE)
target_link_libraries(vp_llama INTERFACE llama ggml)
# SYSTEM so consumers get -isystem: warnings inside llama.h / ggml.h are not reported against our code.
target_include_directories(vp_llama SYSTEM INTERFACE ${VP_LLAMA_DIR}/include ${VP_LLAMA_DIR}/ggml/include)
target_compile_definitions(vp_llama INTERFACE VP_WITH_LLM=1)
