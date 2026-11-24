// Threads, memory and system information. For now these forward to the host unchanged and are
// marked `passthrough`: memory still lacks the console's flags and physical pages, and threads
// and topology still lack the console-to-host processor mapping (plan, sections 5.2 and 5.3).
// Thread start and exit already reach title modules through the launcher's TLS callback.

#include <windows.h>
#include <process.h>

#include "runtime/hle/registry.h"

namespace am::rt::hle {

void add_system(std::vector<Function>& out) {
#define AM_PASS(name) Function{nullptr, #name, reinterpret_cast<void*>(&::name), true}
  out.insert(out.end(), {
      AM_PASS(CreateThread), AM_PASS(ExitThread), AM_PASS(_beginthreadex), AM_PASS(_beginthread),
      AM_PASS(_endthreadex), AM_PASS(SetThreadAffinityMask), AM_PASS(SetThreadGroupAffinity),
      AM_PASS(GetThreadGroupAffinity), AM_PASS(SetThreadIdealProcessor), AM_PASS(SetThreadIdealProcessorEx),
      AM_PASS(GetThreadIdealProcessorEx), AM_PASS(SetThreadSelectedCpuSets), AM_PASS(SetProcessAffinityMask),
      AM_PASS(GetProcessAffinityMask), AM_PASS(VirtualAlloc), AM_PASS(VirtualAllocEx), AM_PASS(VirtualFree),
      AM_PASS(VirtualFreeEx), AM_PASS(VirtualQuery), AM_PASS(VirtualQueryEx), AM_PASS(VirtualProtect),
      AM_PASS(VirtualProtectEx), AM_PASS(GlobalMemoryStatusEx), AM_PASS(GetLogicalProcessorInformation),
      AM_PASS(GetLogicalProcessorInformationEx), AM_PASS(GetSystemInfo), AM_PASS(GetNativeSystemInfo),
      AM_PASS(GetSystemCpuSetInformation), AM_PASS(GetActiveProcessorCount), AM_PASS(GetMaximumProcessorCount),
  });
#undef AM_PASS
}

}  // namespace am::rt::hle
