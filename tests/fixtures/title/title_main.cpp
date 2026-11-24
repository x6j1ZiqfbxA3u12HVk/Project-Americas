// A stand-in "console title" built from our own code, run by americas.exe in the loader tests.
// The exit code is the number of failed checks.

#include <windows.h>
#include <immintrin.h>
#include <ammintrin.h>  // after immintrin.h: it uses its vector types

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

extern "C" IMAGE_DOS_HEADER __ImageBase;

// From xmem.dll (a console module we do not implement yet: the loader binds a stub returning 0).
extern "C" __declspec(dllimport) void* XMemAlloc(size_t size, unsigned long long attributes);
// From the title DLL beside this executable.
extern "C" __declspec(dllimport) int am_fixture_dll_value();
extern "C" __declspec(dllimport) int am_fixture_dll_tls_on_new_thread();
extern "C" __declspec(dllimport) int am_fixture_dll_attach_count();

namespace {

thread_local int t_value = 42;
volatile LONG g_process_attach = 0;
volatile LONG g_thread_attach = 0;
int g_target = 5;
int* g_pointer = &g_target;  // needs a relocation when the image moves

void NTAPI title_tls_callback(PVOID, DWORD reason, PVOID) {
  if (reason == DLL_PROCESS_ATTACH) InterlockedIncrement(&g_process_attach);
  if (reason == DLL_THREAD_ATTACH) InterlockedIncrement(&g_thread_attach);
}

int g_failures = 0;
void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

bool seh_catches_access_violation() {
  __try {
    *static_cast<volatile int*>(nullptr) = 1;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return true;
  }
  return false;
}

[[noreturn]] void throw_deep(int depth) {
  if (depth == 0) throw std::runtime_error("deep");
  throw_deep(depth - 1);
}

}  // namespace

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:am_fixture_tls_callback")
#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK am_fixture_tls_callback = title_tls_callback;

int main(int argc, char** argv) {
  check(t_value == 42, "static TLS: initial value on the main thread");
  t_value = 7;
  int seen_on_thread = 0;
  std::thread([&] { seen_on_thread = t_value; }).join();
  check(seen_on_thread == 42, "static TLS: initial value on a new thread");
  check(t_value == 7, "static TLS: the main thread keeps its own value");
  check(g_process_attach == 1, "TLS callback: process attach, once");
  check(g_thread_attach >= 1, "TLS callback: thread attach");

  bool caught = false;
  try {
    throw_deep(8);
  } catch (const std::exception& e) {
    caught = std::strcmp(e.what(), "deep") == 0;
  }
  check(caught, "C++ exception through 8 frames");
  check(seh_catches_access_violation(), "SEH catches an access violation");

  check(GetModuleHandleW(nullptr) == reinterpret_cast<HMODULE>(&__ImageBase), "GetModuleHandle(NULL) is the title");
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  const size_t n = std::strlen(path);
  check(n > 17 && _stricmp(path + n - 17, "am_test_title.exe") == 0, "GetModuleFileName(NULL) is the title's file");
  check(argc == 2 && std::strcmp(argv[1], "--fixture-arg") == 0, "the title's own arguments");
  check(std::strstr(GetCommandLineA(), "--fixture-arg") != nullptr, "GetCommandLine is the title's");
  check(*g_pointer == 5 && g_pointer == &g_target, "relocated pointers");

  // Console (Zen 2) instructions in ordinary compiled code: patched or emulated on CPUs without them.
  volatile long long in_a = 0x1122334455667788LL, in_b = 0xAB;
  const __m128i a = _mm_cvtsi64_si128(in_a), b = _mm_cvtsi64_si128(in_b);
  check(_mm_cvtsi128_si64(_mm_inserti_si64(a, b, 8, 16)) == 0x1122334455AB7788LL, "SSE4a INSERTQ");
  check(_mm_cvtsi128_si64(_mm_extracti_si64(a, 16, 8)) == 0x6677, "SSE4a EXTRQ");
  float f = 0;
  double g = 0;
  _mm_stream_ss(&f, _mm_set_ss(2.5f));
  _mm_stream_sd(&g, _mm_set_sd(4.25));
  _mm_sfence();
  check(f == 2.5f && g == 4.25, "SSE4a MOVNTSS / MOVNTSD");
  _mm_clwb(&f);
  check(true, "CLWB");

  check(XMemAlloc(64, 0) == nullptr, "unimplemented console import returns 0");
  check(am_fixture_dll_value() == 1234, "title DLL export");
  check(am_fixture_dll_attach_count() == 1, "title DLL: DllMain process attach, once");
  check(am_fixture_dll_tls_on_new_thread() == 99, "title DLL: static TLS on a new thread");

  HMODULE dll = GetModuleHandleW(L"am_test_title_dll.dll");
  using ValueFn = int (*)();
  auto value = reinterpret_cast<ValueFn>(GetProcAddress(dll, "am_fixture_dll_value"));
  check(dll != nullptr && value != nullptr && value() == 1234, "GetModuleHandle + GetProcAddress on a title DLL");

  std::printf("%d check(s) failed\n", g_failures);
  return g_failures;
}
