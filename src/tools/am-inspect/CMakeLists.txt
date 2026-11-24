add_executable(am-inspect main.cpp)
target_link_libraries(am-inspect PRIVATE am_pe am_loader)
