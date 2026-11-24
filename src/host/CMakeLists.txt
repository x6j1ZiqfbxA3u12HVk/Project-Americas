add_library(am_host STATIC cpu.cpp dxc.cpp dxc_compile.cpp gpu.cpp system.cpp)
target_link_libraries(am_host PUBLIC am_core PRIVATE d3d12 dxgi dxguid advapi32)
