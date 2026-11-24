add_executable(am_tests
  test_main.cpp
  core_tests.cpp
  cpu_emulation_tests.cpp
  cpu_tests.cpp
  pe_tests.cpp
  policy_tests.cpp
  x86_tests.cpp)
target_link_libraries(am_tests PRIVATE am_core am_pe am_host am_loader am_x86 am_runtime)

add_test(NAME unit COMMAND am_tests)
# The tools run on this machine: the probe finds a usable GPU, the inspector reads a PE we built.
add_test(NAME probe_runs COMMAND am-probe)
add_test(NAME inspect_own_binary COMMAND am-inspect $<TARGET_FILE:am-inspect> --no-hash)

# The loader runs our fixture title, at its preferred base and moved elsewhere.
add_subdirectory(fixtures/title)
foreach(variant IN ITEMS "" "--force-relocation")
  if(variant STREQUAL "")
    set(name loader_runs_title)
  else()
    set(name loader_runs_title_relocated)
  endif()
  add_test(NAME ${name} COMMAND americas --title $<TARGET_FILE:am_test_title> ${variant} -- --fixture-arg)
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "AMERICAS_ROOT=${PROJECT_BINARY_DIR}/test-root" TIMEOUT 60)
endforeach()
