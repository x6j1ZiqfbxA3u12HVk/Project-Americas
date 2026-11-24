# americas.exe: TLS slot 0 for the title's executable, a TLS callback, and nothing else.
add_executable(americas main.cpp)
target_include_directories(americas PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(americas PRIVATE am_options)
# A large main-thread stack: the title runs on it. A base far from 0x140000000, where title
# executables want to be mapped.
target_link_options(americas PRIVATE /STACK:16777216 /BASE:0x7FF700000000)
add_dependencies(americas americas_rt)
