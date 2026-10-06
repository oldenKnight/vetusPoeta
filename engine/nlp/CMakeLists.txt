# vp_nlp: EN/ES tokeniser, perceptron tagger, arc-eager parser, lemmatiser hook (.vpt reader). DESIGN.md 10.1, 17.
add_library(vp_nlp STATIC
  src/feature_strings.cpp
  src/lemma.cpp
  src/model.cpp
  src/morph.cpp
  src/parser.cpp
  src/pipeline.cpp
  src/tagger.cpp
  src/tokenizer.cpp)
target_include_directories(vp_nlp PUBLIC include PRIVATE src)
target_link_libraries(vp_nlp PUBLIC vp_core)
target_compile_features(vp_nlp PUBLIC cxx_std_17)
