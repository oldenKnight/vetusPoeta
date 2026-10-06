# engine iii: the online check (DESIGN §1.3, §12). The only module with network code. On Windows it uses WinHTTP
# (system DLL); elsewhere a dev-only transport that runs the curl program (never shipped).
add_library(vp_online STATIC
  src/parse.cpp
  src/wiktionary.cpp
  src/transport_posix_dev.cpp
  src/transport_winhttp.cpp)
target_include_directories(vp_online PUBLIC include)
target_link_libraries(vp_online PUBLIC vp_core vp_third_party)
target_compile_features(vp_online PUBLIC cxx_std_17)
if(WIN32)
  target_link_libraries(vp_online PRIVATE winhttp)
endif()

# Guard for DESIGN §1.3 "no other module includes a socket header": scans every other engine library for network
# symbols (sockets, resolvers, WinHTTP/WinINet, libcurl) and process spawning (the dev transport's way out).
if(VP_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    enable_testing()
    add_test(NAME vp_online_symbols
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_symbols.py ${CMAKE_BINARY_DIR})
  endif()
endif()
