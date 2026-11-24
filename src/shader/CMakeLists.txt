add_library(am_shader STATIC container.cpp pack.cpp)
target_link_libraries(am_shader PUBLIC am_core am_formats)
