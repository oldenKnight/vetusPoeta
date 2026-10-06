# vp_rules: rule engine (DESIGN.md §6, §10). C1 (Latin side): curated data loaders, morphology (analyse/generate +
# paradigm fallback), Latin realisation primitives and the checker. makeEngine() arrives with C2.
file(GLOB VP_RULES_SRC CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/curated/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/morph/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/realise_la/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/check/*.cpp)
add_library(vp_rules STATIC ${VP_RULES_SRC})
target_include_directories(vp_rules PUBLIC include PRIVATE src)
target_link_libraries(vp_rules PUBLIC vp_lex vp_core)
target_compile_features(vp_rules PUBLIC cxx_std_17)
