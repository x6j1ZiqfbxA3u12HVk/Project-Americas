#include "loader/import_policy.h"
#include "test.h"

using namespace am::loader;

TEST_CASE(policy_console_modules_are_ours) {
  const ImportPolicy policy;
  CHECK(policy.route("d3d12_xs.dll", "D3D12XboxCreateDevice") == Route::Ours);
  CHECK(policy.route("XMEM.DLL", "XMemAlloc") == Route::Ours);
  CHECK(policy.route("GameInput.dll", "") == Route::Ours);
}

TEST_CASE(policy_wraps_functions_that_need_console_behaviour) {
  const ImportPolicy policy;
  CHECK(policy.route("api-ms-win-core-file-l1-1-0.dll", "CreateFileW") == Route::Wrap);
  CHECK(policy.route("KERNEL32.dll", "CreateFileW") == Route::Wrap);
  CHECK(policy.route("api-ms-win-core-memory-l1-1-0.dll", "VirtualAlloc") == Route::Wrap);
  CHECK(policy.route("api-ms-win-crt-stdio-l1-1-0.dll", "fopen") == Route::Wrap);
  CHECK(policy.route("WS2_32.dll", "connect") == Route::Wrap);
  CHECK(wrap_reason("CreateFileW") == "console paths");
  CHECK(wrap_reason("ReadFile").empty());
}

TEST_CASE(policy_passes_plain_host_functions) {
  const ImportPolicy policy;
  CHECK(policy.route("api-ms-win-core-file-l1-1-0.dll", "ReadFile") == Route::Host);
  CHECK(policy.route("api-ms-win-crt-math-l1-1-0.dll", "sqrt") == Route::Host);
  CHECK(policy.route("ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow") == Route::Host);
  CHECK(policy.route("WS2_32.dll", "") == Route::Host);  // an ordinal nobody could name
}

TEST_CASE(policy_prefers_the_titles_own_modules) {
  const ImportPolicy policy({"MSVCP140.dll", "Bink2Scarlett.dll"});
  CHECK(policy.route("msvcp140.dll", "_Fiopen") == Route::Title);
  CHECK(policy.route("bink2scarlett.dll", "BinkOpen") == Route::Title);
  CHECK(policy.route("vcruntime140.dll", "memcpy") == Route::Host);  // not shipped here: the host's
  CHECK(policy.route("unknown_vendor.dll", "f") == Route::Unresolved);
}
