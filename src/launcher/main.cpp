// americas.exe: the launcher. It holds no logic of its own, only what has to live in the
// executable the process starts from:
//  * static TLS slot 0, reserved for the title's executable (its code expects slot 0);
//  * a TLS callback, through which every thread start and exit reaches the title's modules.
// Everything else is in americas_rt.dll, next to this file.
//
// This file must not use thread_local or __declspec(thread) for anything but the carrier:
// the title's TLS template overwrites this module's whole TLS block.

#include <windows.h>

#include <cstdio>
#include <filesystem>

#include "runtime/launcher_api.h"

namespace {

// Slot 0. Its size bounds the title executable's TLS block.
__declspec(thread) alignas(64) unsigned char g_title_tls[64 * 1024];

am::rt::ThreadHook g_thread_hook = nullptr;

void NTAPI on_thread(PVOID module, DWORD reason, PVOID) {
  const am::rt::ThreadHook hook = g_thread_hook;
  if (hook && (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH)) hook(module, reason);
}

}  // namespace

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:am_tls_callback")
#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK am_tls_callback = on_thread;

int wmain(int argc, wchar_t** argv) {
  g_title_tls[0] = 0;  // keeps the carrier in the image
  // First, before any DLL of ours loads: hold the range title executables are mapped at.
  void* window = VirtualAlloc(reinterpret_cast<void*>(am::rt::kImageWindow), am::rt::kImageWindowSize, MEM_RESERVE,
                              PAGE_NOACCESS);

  wchar_t self[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  const std::filesystem::path runtime = std::filesystem::path(self).parent_path() / L"americas_rt.dll";
  HMODULE rt = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!rt) {
    std::fprintf(stderr, "cannot load %ls (error %lu)\n", runtime.c_str(), GetLastError());
    return 1;
  }
  auto launch = reinterpret_cast<am_launch_fn>(GetProcAddress(rt, "am_launch"));
  if (!launch) {
    std::fprintf(stderr, "americas_rt.dll has no am_launch\n");
    return 1;
  }
  am::rt::LauncherInfo info;
  info.thread_hook = &g_thread_hook;
  info.argc = argc;
  info.argv = argv;
  info.image_window = window;
  return launch(&info);
}
