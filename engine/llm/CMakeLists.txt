# engine/llm: engine ii (DESIGN §11). Added by the top level only when VP_WITH_LLM=ON; with OFF, engine/cli defines
# an INTERFACE `vp_llm` that carries only the include directory, and vp/llm.h compiles to its stub.
add_library(vp_llm STATIC
  src/cpu.cpp
  src/gate.cpp
  src/llm.cpp)
target_include_directories(vp_llm PUBLIC include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vp_llm PUBLIC vp_core vp_lex PRIVATE vp_llama Threads::Threads)
target_compile_definitions(vp_llm PUBLIC VP_LLM_REAL=1)

# Protocol test of the model.* commands against the real vpengine (python3 stdlib). vpengine is defined later
# (engine/cli); the generator expression resolves at generate time.
if(VP_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    enable_testing()
    add_test(NAME vp_llm_protocol
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_model_protocol.py
                     $<TARGET_FILE:vpengine> ${CMAKE_BINARY_DIR}/testtmp/llm_protocol)
    set_tests_properties(vp_llm_protocol PROPERTIES TIMEOUT 300)
  endif()
endif()
