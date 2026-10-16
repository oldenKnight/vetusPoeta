# vpengine: the engine executable (DESIGN.md §9). `vp_cli_core` holds everything but main() so the doctest
# executable can test the pure helpers (engine/tests/test_cli.cpp).
option(VP_HAVE_RULES "Link engine/rules (vp::rules::makeEngine(EngineConfig)); the stub stays available with --stub" ON)

add_library(vp_cli_core STATIC
  src/cmd_llm.cpp
  src/cmd_serve.cpp
  src/cmd_tools.cpp
  src/locate.cpp
  src/online_mock.cpp
  src/serve_io.cpp
  src/rules_stub.cpp
  src/server.cpp
  src/server_cues.cpp
  src/server_engine.cpp
  src/server_export.cpp
  src/views.cpp)
target_include_directories(vp_cli_core PUBLIC src ${CMAKE_SOURCE_DIR}/engine/rules/include)
target_link_libraries(vp_cli_core PUBLIC vp_core vp_subs vp_lex vp_third_party Threads::Threads)
# Engine ii (engine/llm). With VP_WITH_LLM=OFF that module is not built: an INTERFACE target carries its header,
# which then compiles to a stub answering model_missing (the test executable links it the same way).
if(NOT TARGET vp_llm)
  add_library(vp_llm INTERFACE)
  target_include_directories(vp_llm INTERFACE ${CMAKE_SOURCE_DIR}/engine/llm/include)
endif()
target_link_libraries(vp_cli_core PUBLIC vp_llm)
if(VP_HAVE_RULES AND TARGET vp_rules)
  target_link_libraries(vp_cli_core PUBLIC vp_rules)
  target_compile_definitions(vp_cli_core PRIVATE VP_HAVE_RULES=1)
endif()
if(WIN32)
  target_link_libraries(vp_cli_core PUBLIC shell32)
endif()
# Engine iii (engine/online, the only network code): online.test and the translate job's online check.
target_link_libraries(vp_cli_core PUBLIC vp_online)

add_executable(vpengine src/main.cpp)
target_link_libraries(vpengine PRIVATE vp_cli_core)

# engine/tests/CMakeLists.txt globs test_cli.cpp into vp_tests, which is created after this directory: link the
# helpers into it once the whole tree is configured.
function(vp_cli_link_tests)
  if(TARGET vp_tests)
    target_link_libraries(vp_tests PRIVATE vp_cli_core)
  endif()
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL vp_cli_link_tests)

# The protocol test (python3 stdlib). The top level enables testing only after the modules are added, so this
# directory enables it itself (add_test is a no-op otherwise).
if(VP_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    enable_testing()
    add_test(NAME vpengine_server
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/engine/tests/test_server.py
                     $<TARGET_FILE:vpengine> ${CMAKE_BINARY_DIR}/testtmp/server)
    set_tests_properties(vpengine_server PROPERTIES TIMEOUT 1200)
    if(VP_SANITIZE)   # the local-model part (llama.cpp scoring) runs in release builds only
      set_tests_properties(vpengine_server PROPERTIES ENVIRONMENT "VP_LLM_SKIP_MODEL=1")
    endif()
  endif()
endif()
