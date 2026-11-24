add_executable(am-shaders main.cpp survey.cpp translate_cmd.cpp)
target_link_libraries(am-shaders PRIVATE am_shader am_host)
