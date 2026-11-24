// The fixture title's DLL: exports, DllMain and its own static TLS.

#include <windows.h>

#include <thread>

namespace {
thread_local int t_dll_value = 99;
volatile LONG g_attach = 0;
}  // namespace

extern "C" __declspec(dllexport) int am_fixture_dll_value() { return 1234; }

extern "C" __declspec(dllexport) int am_fixture_dll_attach_count() { return g_attach; }

extern "C" __declspec(dllexport) int am_fixture_dll_tls_on_new_thread() {
  t_dll_value = 5;  // must not leak into other threads
  int seen = 0;
  std::thread([&] { seen = t_dll_value; }).join();
  return seen;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) InterlockedIncrement(&g_attach);
  return TRUE;
}
