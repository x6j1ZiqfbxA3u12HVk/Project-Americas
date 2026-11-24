add_library(am_runtime STATIC
  console.cpp
  cpu_compat.cpp
  host_patch.cpp
  image.cpp
  log.cpp
  process.cpp
  stubs.cpp
  title_config.cpp
  tls.cpp
  files/console_paths.cpp
  gdk/catalog.cpp
  gdk/offline.cpp
  gdk/runtime.cpp
  gdk/task_queue.cpp
  gdk/task_queue_api.cpp
  memory/console_flags.cpp
  storage/dstorage.cpp
  memory/default_alloc.cpp
  memory/title_memory.cpp
  hle/d3d12x.cpp
  d3d12x/descriptor_heap.cpp
  d3d12x/device.cpp
  d3d12x/dxgi.cpp
  d3d12x/formats.cpp
  d3d12x/frame.cpp
  d3d12x/layout.cpp
  d3d12x/list.cpp
  d3d12x/object.cpp
  d3d12x/pipeline.cpp
  d3d12x/present.cpp
  d3d12x/queue.cpp
  d3d12x/resource.cpp
  d3d12x/root_signature.cpp
  d3d12x/views.cpp
  d3d12x/xg.cpp
  hle/files.cpp
  present/presenter.cpp
  present/window.cpp
  input/game_input.cpp
  hle/identity.cpp
  hle/network.cpp
  hle/registry.cpp
  hle/system.cpp
  hle/xmem.cpp)
target_link_libraries(am_runtime PUBLIC am_core am_pe am_loader am_x86 am_formats PRIVATE ws2_32 winhttp d3d12 dxgi)

# The runtime proper, loaded by americas.exe.
add_library(americas_rt SHARED rt_main.cpp)
target_link_libraries(americas_rt PRIVATE am_runtime am_host am_tls_1 am_tls_2 am_tls_3 am_tls_4)
# A map file beside the DLL: crash and hang reports give its addresses as americas_rt.dll+offset.
target_link_options(americas_rt PRIVATE /MAP)
