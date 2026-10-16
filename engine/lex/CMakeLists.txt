# vp_lex: read-only `.vpl` lexicon reader (DESIGN.md §5, §5.1). Memory map and SHA-256 come from vp_core.
add_library(vp_lex STATIC src/lexicon.cpp)
target_include_directories(vp_lex PUBLIC include PRIVATE src)
target_link_libraries(vp_lex PUBLIC vp_core)
target_compile_features(vp_lex PUBLIC cxx_std_17)
