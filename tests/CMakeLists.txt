add_executable(am_tests
  test_main.cpp
  core_tests.cpp
  cpu_tests.cpp
  pe_tests.cpp
  policy_tests.cpp)
target_link_libraries(am_tests PRIVATE am_core am_pe am_host am_loader)

add_test(NAME unit COMMAND am_tests)
# The tools run on this machine: the probe finds a usable GPU, the inspector reads a PE we built.
add_test(NAME probe_runs COMMAND am-probe)
add_test(NAME inspect_own_binary COMMAND am-inspect $<TARGET_FILE:am-inspect> --no-hash)
