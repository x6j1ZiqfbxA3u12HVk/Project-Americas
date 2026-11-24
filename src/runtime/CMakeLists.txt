add_library(am_runtime STATIC
  cpu_compat.cpp
  host_patch.cpp
  image.cpp
  log.cpp
  process.cpp
  stubs.cpp
  tls.cpp
  hle/identity.cpp
  hle/network.cpp
  hle/registry.cpp
  hle/system.cpp)
target_link_libraries(am_runtime PUBLIC am_core am_pe am_loader am_x86 PRIVATE ws2_32 winhttp)

# The runtime proper, loaded by americas.exe.
add_library(americas_rt SHARED rt_main.cpp)
target_link_libraries(americas_rt PRIVATE am_runtime am_tls_1 am_tls_2 am_tls_3 am_tls_4)
