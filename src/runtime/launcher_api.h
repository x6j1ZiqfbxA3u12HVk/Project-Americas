#pragma once

// The contract between americas.exe (the launcher) and americas_rt.dll (the runtime).

#include <cstdint>

namespace am::rt {

using ThreadHook = void (*)(void* module, unsigned long reason);

// Where title executables want to be mapped. The launcher reserves this range before anything
// else can take it; the runtime releases it just before it maps the executable there.
constexpr uint64_t kImageWindow = 0x140000000;
constexpr uint64_t kImageWindowSize = 0x40000000;

struct LauncherInfo {
  uint32_t version = 2;
  // The launcher's TLS callback calls *thread_hook (when set) on every thread start and exit.
  ThreadHook* thread_hook = nullptr;
  int argc = 0;
  wchar_t** argv = nullptr;
  void* image_window = nullptr;  // the reservation at kImageWindow; null when it was taken
};

}  // namespace am::rt

// Exported by americas_rt.dll. Returns the process exit code when the title returns.
extern "C" using am_launch_fn = int (*)(const am::rt::LauncherInfo* info);
