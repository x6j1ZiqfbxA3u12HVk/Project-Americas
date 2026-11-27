add_library(am_shader STATIC console_program.cpp container.cpp dxil/bitcode.cpp dxil/dxil.cpp image_bindings.cpp pack.cpp rdna2.cpp
            stage_io.cpp translate.cpp)
target_link_libraries(am_shader PUBLIC am_core am_formats)
