add_library(am_x86 STATIC code_scan.cpp decoder.cpp requirement.cpp)
target_link_libraries(am_x86 PUBLIC am_core am_host am_pe)
