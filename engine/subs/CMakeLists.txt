add_library(vp_subs STATIC
  src/encoding.cpp
  src/parse.cpp
  src/text.cpp
  src/timing.cpp
  src/write.cpp)
target_include_directories(vp_subs PUBLIC include ${CMAKE_SOURCE_DIR}/engine/core/include PRIVATE src)
target_compile_features(vp_subs PUBLIC cxx_std_17)
