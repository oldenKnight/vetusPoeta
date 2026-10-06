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

# C2 (source side and engine assembly): frame builder, transfer, cue assembly, makeEngine(). Links vp_nlp and vp_subs.
file(GLOB VP_RULES_C2_SRC CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/frame/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/transfer/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cue/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.cpp)
target_sources(vp_rules PRIVATE ${VP_RULES_C2_SRC})
target_link_libraries(vp_rules PUBLIC vp_nlp vp_subs)

# C9 (Greek side): Greek morphology helpers, realisation primitives and checker. Guarded so the block is idempotent.
if(NOT VP_RULES_GRC_ADDED)
  set(VP_RULES_GRC_ADDED ON)
  file(GLOB VP_RULES_GRC_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/realise_grc/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/check_grc/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/morph_grc/*.cpp)
  target_sources(vp_rules PRIVATE ${VP_RULES_GRC_SRC})
endif()

# C11 (Latin -> English / Spanish): la2x/ analyser, interlinear and readable sentence. Guarded so the block is idempotent.
if(NOT VP_RULES_LA2X_ADDED)
  set(VP_RULES_LA2X_ADDED ON)
  file(GLOB VP_RULES_LA2X_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/la2x/*.cpp)
  target_sources(vp_rules PRIVATE ${VP_RULES_LA2X_SRC})
endif()

# C12 (Greek engine path): EN/ES -> Greek transfer, Greek -> EN/ES (grc2x), the Greek engine path. Guarded so the block
# is idempotent.
if(NOT VP_RULES_GRC2_ADDED)
  set(VP_RULES_GRC2_ADDED ON)
  file(GLOB VP_RULES_GRC2_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transfer_grc/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/grc2x/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine_grc/*.cpp)
  target_sources(vp_rules PRIVATE ${VP_RULES_GRC2_SRC})
endif()
