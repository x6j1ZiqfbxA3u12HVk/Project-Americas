add_library(am_shader STATIC console_program.cpp container.cpp pack.cpp rdna2.cpp translate.cpp)
target_link_libraries(am_shader PUBLIC am_core am_formats)
