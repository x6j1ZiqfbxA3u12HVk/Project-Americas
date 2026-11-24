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

// Console modules: our implementations, bound by the loader.
extern "C" __declspec(dllimport) void* XMemAlloc(size_t size, unsigned long long attributes);
extern "C" __declspec(dllimport) void XMemFree(void* address, unsigned long long attributes);
extern "C" __declspec(dllimport) void* XMemVirtualAlloc(void* address, size_t size, unsigned long type,
                                                       unsigned long long flags, unsigned long protect);
extern "C" __declspec(dllimport) int XMemVirtualQuery(HANDLE process, const void* address, void* info);
extern "C" __declspec(dllimport) int XMemAllocatePhysicalPages(unsigned long long flags, unsigned long long* count,
                                                               unsigned long long* pages);
extern "C" __declspec(dllimport) int XMemFreePhysicalPages(unsigned long long count, const unsigned long long* pages);
extern "C" __declspec(dllimport) void* XMemMapPhysicalPages(void* address, unsigned long long count,
                                                           const unsigned long long* pages);
extern "C" __declspec(dllimport) int XMemGetWorkingSetStatistics(unsigned long which, void* out);
extern "C" __declspec(dllimport) long D3D12ConfigureVirtualMemoryX(void* config);
// A console function we do not implement: the loader binds a stub returning 0.
extern "C" __declspec(dllimport) void* AcpHalCreate(void* a, void* b);
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

// The game's memory set-up in small: a CPU range and a GPU range of 64 KiB pages, physical pages
// mapped into both, plus the console's VirtualAlloc flags.
void check_console_memory() {
  constexpr unsigned long kPages64K = 0x20400000, kPages2M = 0x20000000;
  constexpr size_t kRange = 4u << 20;
  check(D3D12ConfigureVirtualMemoryX(nullptr) < 0, "D3D12ConfigureVirtualMemoryX refuses no block");
  unsigned config[8] = {0, 3, 0, 0, 0, 0, 0, 0};
  check(D3D12ConfigureVirtualMemoryX(config) == 0, "D3D12ConfigureVirtualMemoryX accepts the game's block");
  auto* cpu = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, kRange, kPages64K | MEM_RESERVE, 0x21, PAGE_READWRITE));
  auto* gpu = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, kRange, kPages64K | MEM_RESERVE, 3, 0x801));
  const auto low = [](const void* p) { return reinterpret_cast<uintptr_t>(p) >> 40 == 0; };
  check(cpu && gpu && low(cpu) && low(gpu), "XMemVirtualAlloc: 64 KiB page ranges below 2^40");
  unsigned long long count = 3, pages[3] = {};
  check(XMemAllocatePhysicalPages(0x380, &count, pages) && count == 3, "XMemAllocatePhysicalPages");
  check(XMemMapPhysicalPages(cpu, 3, pages) == cpu, "XMemMapPhysicalPages");
  cpu[2 * 65536 + 7] = 0x77;
  // The GPU range shows the same page; the CPU may not touch it there (PAGE_NOACCESS).
  check(XMemMapPhysicalPages(gpu, 1, pages + 2) == gpu, "XMemMapPhysicalPages: a second address");
  DWORD old = 0;
  check(VirtualProtect(gpu, 65536, PAGE_READONLY, &old) && gpu[7] == 0x77, "physical page seen at both addresses");
  unsigned char info[0x40] = {};
  check(XMemVirtualQuery(GetCurrentProcess(), cpu, info) && *reinterpret_cast<DWORD*>(info + 0x20) == MEM_COMMIT &&
            *reinterpret_cast<unsigned long long*>(info + 0x30) == 0x21,
        "XMemVirtualQuery: committed, with the allocation's flags");
  check(VirtualFree(cpu, 0, MEM_RELEASE) && VirtualFree(gpu, 0, MEM_RELEASE), "VirtualFree releases mapped ranges");
  check(XMemFreePhysicalPages(3, pages), "XMemFreePhysicalPages");

  // VirtualAlloc with console bits: 2 MiB pages and a GPU protection.
  void* big = VirtualAlloc(nullptr, 1 << 20, kPages2M | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | 0x2000);
  check(big && reinterpret_cast<uintptr_t>(big) % (2 << 20) == 0, "VirtualAlloc: console 2 MiB pages");
  check(VirtualFree(big, 2 << 20, MEM_RELEASE), "VirtualFree: MEM_RELEASE with the allocation's size");

  void* block = XMemAlloc(64, 12ull << 24);
  check(block && reinterpret_cast<uintptr_t>(block) % 4096 == 0, "XMemAlloc: aligned block (default allocator)");
  XMemFree(block, 12ull << 24);
  unsigned long long stats[15] = {};
  check(XMemGetWorkingSetStatistics(0x20000001, stats) && stats[0] > stats[1] && stats[1] > 0,
        "XMemGetWorkingSetStatistics: a budget above what is used");
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

  check(AcpHalCreate(nullptr, nullptr) == nullptr, "unimplemented console import returns 0");
  check_console_memory();
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
