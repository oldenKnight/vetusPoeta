# One doctest executable for all modules. Each module adds test_<module>.cpp here and links its target below via
# the VP_TEST_LIBS list (append your target when your module exists; do not remove others).
file(GLOB VP_TEST_SRC CONFIGURE_DEPENDS *.cpp)
add_executable(vp_tests ${VP_TEST_SRC})
target_link_libraries(vp_tests PRIVATE vp_third_party Threads::Threads)
foreach(lib vp_core vp_lex vp_subs vp_nlp vp_rules vp_llm vp_online)
  if(TARGET ${lib})
    target_link_libraries(vp_tests PRIVATE ${lib})
  endif()
endforeach()
target_compile_definitions(vp_tests PRIVATE VP_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures" VP_TEST_TMP="${CMAKE_BINARY_DIR}/testtmp")
add_test(NAME vp_tests COMMAND vp_tests)
