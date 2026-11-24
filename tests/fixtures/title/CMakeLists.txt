# The fixture title: an executable and a DLL in a folder of their own, like a game folder.
set(fixture_dir "${PROJECT_BINARY_DIR}/fixtures/title")
set(xmem_lib "${CMAKE_CURRENT_BINARY_DIR}/xmem.lib")

add_custom_command(
  OUTPUT "${xmem_lib}"
  COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_SOURCE_DIR}/xmem_fixture.def" "/out:${xmem_lib}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/xmem_fixture.def"
  COMMENT "Import library for the fixture's xmem.dll")
add_custom_target(am_fixture_xmem DEPENDS "${xmem_lib}")

add_library(am_test_title_dll SHARED title_dll.cpp)
target_link_libraries(am_test_title_dll PRIVATE am_options)
set_target_properties(am_test_title_dll PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${fixture_dir}")

add_executable(am_test_title title_main.cpp)
add_dependencies(am_test_title am_fixture_xmem)
target_link_libraries(am_test_title PRIVATE am_options am_test_title_dll "${xmem_lib}")
set_target_properties(am_test_title PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${fixture_dir}")
