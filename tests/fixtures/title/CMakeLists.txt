# The fixture title: an executable and a DLL in a folder of their own, like a game folder.
set(fixture_dir "${PROJECT_BINARY_DIR}/fixtures/title")

# Import libraries for the console modules the fixture calls, which the loader provides.
set(console_libs)
foreach(module IN ITEMS xmem d3d12 acphal)
  set(def "${CMAKE_CURRENT_SOURCE_DIR}/${module}_fixture.def")
  set(lib "${CMAKE_CURRENT_BINARY_DIR}/${module}_fixture.lib")
  add_custom_command(
    OUTPUT "${lib}"
    COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${def}" "/out:${lib}"
    DEPENDS "${def}"
    COMMENT "Import library for the fixture's ${module}")
  list(APPEND console_libs "${lib}")
endforeach()
add_custom_target(am_fixture_console_libs DEPENDS ${console_libs})

add_library(am_test_title_dll SHARED title_dll.cpp)
target_link_libraries(am_test_title_dll PRIVATE am_options)
set_target_properties(am_test_title_dll PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${fixture_dir}")

add_executable(am_test_title title_main.cpp)
add_dependencies(am_test_title am_fixture_console_libs)
target_link_libraries(am_test_title PRIVATE am_options am_test_title_dll ${console_libs})
set_target_properties(am_test_title PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${fixture_dir}")
