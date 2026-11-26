add_executable(am-shaders main.cpp survey.cpp translate_cmd.cpp)
target_link_libraries(am-shaders PRIVATE am_shader am_host)
# DXC recurses deeply on the largest programs (2 MB of HLSL): every thread gets a 256 MiB
# stack reservation (address space; pages are committed only as used).
target_link_options(am-shaders PRIVATE /STACK:268435456)
