#pragma once

// The contract between americas.exe (the launcher) and americas_rt.dll (the runtime).

#include <cstdint>

namespace am::rt {

using ThreadHook = void (*)(void* module, unsigned long reason);

struct LauncherInfo {
  uint32_t version = 1;
  // The launcher's TLS callback calls *thread_hook (when set) on every thread start and exit.
  ThreadHook* thread_hook = nullptr;
  int argc = 0;
  wchar_t** argv = nullptr;
};

}  // namespace am::rt

// Exported by americas_rt.dll. Returns the process exit code when the title returns.
extern "C" using am_launch_fn = int (*)(const am::rt::LauncherInfo* info);
