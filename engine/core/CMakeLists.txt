# vp_core: utf8 + normalisation, errors, fs utils, mmap RAII, settings, project (.vpoeta) format. DESIGN.md sections 4, 8, 9.
file(GLOB VP_CORE_SRC CONFIGURE_DEPENDS src/*.cpp src/*.c)
add_library(vp_core STATIC ${VP_CORE_SRC})
target_include_directories(vp_core PUBLIC include PRIVATE src)
target_link_libraries(vp_core PUBLIC vp_third_party Threads::Threads)
target_compile_definitions(vp_core PRIVATE VP_APP_VERSION="${PROJECT_VERSION}")
