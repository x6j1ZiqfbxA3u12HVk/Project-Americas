#include "runtime/process.h"

#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <csignal>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "core/json.h"
#include "core/text.h"
#include "runtime/console.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/gdk/runtime.h"
#include "runtime/host_patch.h"
#include "runtime/launcher_api.h"
#include "runtime/log.h"
#include "runtime/memory/title_memory.h"

namespace am::rt {

void on_abort(int);  // the SIGABRT handler (TitleProcess::load)

namespace {

TitleProcess* g_process = nullptr;

std::string normalize_module_name(std::string_view name) {
  std::string n(name);
  const size_t slash = n.find_last_of("\\/");
  if (slash != std::string::npos) n = n.substr(slash + 1);
  if (n.find('.') == std::string::npos) n += ".dll";
  return to_lower_ascii(n);
}

std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
    else out.append(backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  out += L'"';
  return out;
}

LONG WINAPI top_level_filter(EXCEPTION_POINTERS* info) { return g_process ? g_process->crash_filter(info) : 0; }

// ---- AMERICAS_HW_WATCH (diagnosis) ----
// "addr[:len],..." (hex, up to four; len 1, 2, 4 or 8, default 8): a data breakpoint on each, in
// every thread of the process (a watcher arms new threads every 20 ms). Each write is logged
// with the value now there, the writing thread and its stack.

struct HwWatch {
  uint64_t address = 0;
  uint32_t length = 8;
};
HwWatch g_hw_watches[4];
std::atomic<int> g_hw_watch_count{0};
std::atomic<int> g_hw_generation{0};  // the watches changed: every thread is armed again
volatile LONG g_hw_hits = 0;

LONG CALLBACK on_hw_watch(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* ctx = info->ContextRecord;
  const DWORD64 which = ctx->Dr6 & 0xF;
  if (!which) return EXCEPTION_CONTINUE_SEARCH;
  ctx->Dr6 = 0;
  if (InterlockedIncrement(&g_hw_hits) > 400 || !g_process) return EXCEPTION_CONTINUE_EXECUTION;
  for (int k = 0; k < g_hw_watch_count; ++k) {
    if (!(which & (1ull << k))) continue;
    const HwWatch& w = g_hw_watches[k];
    uint64_t now = 0;
    std::memcpy(&now, reinterpret_cast<const void*>(w.address), w.length);
    std::string text = "hw watch: 0x" + hex(w.address) + " = 0x" + hex(now) + ", written by thread " +
                       std::to_string(GetCurrentThreadId()) + " before " + g_process->describe_address(ctx->Rip);
    char regs[256];
    std::snprintf(regs, sizeof regs, "\n  rax %016llx rbx %016llx rcx %016llx rdx %016llx rsi %016llx rdi %016llx", ctx->Rax,
                  ctx->Rbx, ctx->Rcx, ctx->Rdx, ctx->Rsi, ctx->Rdi);
    text += regs;
    text += g_process->describe_stack(*ctx);
    log_line(text);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void hw_watch_thread() {
  SetThreadDescription(GetCurrentThread(), L"americas hw watch");
  DWORD64 dr7 = 0;
  int generation = -1;
  std::vector<DWORD> armed;
  const DWORD self = GetCurrentThreadId();
  for (;;) {
    if (const int now = g_hw_generation.load(); now != generation) {
      generation = now;
      armed.clear();
      dr7 = 0;
      for (int k = 0; k < g_hw_watch_count; ++k) {
        const uint32_t len = g_hw_watches[k].length;
        const DWORD64 len_bits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
        dr7 |= 1ull << (2 * k);                           // local enable
        dr7 |= (1ull | (len_bits << 2)) << (16 + 4 * k);  // RW = write, LEN
      }
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      THREADENTRY32 te{};
      te.dwSize = sizeof te;
      for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        if (std::find(armed.begin(), armed.end(), te.th32ThreadID) != armed.end()) continue;
        armed.push_back(te.th32ThreadID);
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
          CONTEXT c{};
          c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
          if (GetThreadContext(t, &c)) {
            DWORD64* dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
            for (int k = 0; k < g_hw_watch_count; ++k) *dr[k] = g_hw_watches[k].address;
            c.Dr7 = dr7;
            SetThreadContext(t, &c);
          }
          ResumeThread(t);
        }
        CloseHandle(t);
      }
      CloseHandle(snapshot);
    }
    Sleep(20);
  }
}

bool add_hw_watch(uint64_t address, uint32_t length);

// ---- AMERICAS_CLAMP_PAGE_RELEASE (diagnosis) ----
// The title's 64 KiB page allocator keeps one 8-byte entry a page (the array's address at
// game+0x71bfb08): a size class in bits 15-19, a reference count in bits 0-14. Its release
// (game+0xe8890) drops one reference on the block holding a page and frees the block at 0. In
// the loading after Confirm some pages are released more often than taken; the count wraps and
// the free lists break (a thread then spins in game+0x4f8f120). With this switch, the release
// is ours: the same steps, but a release of a page whose count is already 0 is refused and
// logged with its stack.

constexpr uint64_t kPageTableRva = 0x71bfb08, kPageReleaseRva = 0xe8890, kPageFreeRva = 0x221b7d0;
uint8_t* g_title_base = nullptr;

void clamped_page_release(uint32_t index) {
  uint64_t* const table = *reinterpret_cast<uint64_t**>(g_title_base + kPageTableRva);
  const uint32_t size_class = static_cast<uint32_t>(table[index] >> 15) & 0x1F;
  const uint32_t mask = size_class >= 0x18 ? 0xFFFFFFFFu : (0xFFFFFFFFu << size_class);
  const uint32_t head = index & mask;
  const uint64_t entry = table[head];
  if ((entry & 0x7FFF) == 0) {
    static std::atomic<int> told{0};
    const int n = told.fetch_add(1);
    if (n < 40) {
      void* frames[8] = {};
      const USHORT got = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
      std::string text;
      for (USHORT k = 0; k < got; ++k) text += " " + g_process->describe_address(reinterpret_cast<uint64_t>(frames[k]));
      logf("pages: a release of page 0x%x (block 0x%x, entry 0x%llx) with no reference left: refused;%s", index, head,
           static_cast<unsigned long long>(entry), text.c_str());
    } else if (n == 40 || n == 1000 || n == 100000) {
      logf("pages: %d releases with no reference left refused so far", n + 1);
    }
    return;
  }
  const uint64_t count = (entry - 1) & 0x7FFF;
  table[head] = (entry & ~0x7FFFull) | count;
  if (!count) reinterpret_cast<void (*)(uint64_t, uint64_t*)>(g_title_base + kPageFreeRva)(head, table);
}

// The title releases an object's pages (game+0x41db90): its GPU address at +0x18 (64 KiB
// aligned down), its size (u32 at +0x8 times the low 12 bits of the u16 at +0xc, 64 KiB
// aligned up), moved from the pool's GPU view to its CPU view (+ [game+0x9e1dac0] -
// [game+0x9e1dad0]) and released (game+0x41dbe0). Ours does the same and logs the releases
// larger than the allocator's block they start in.
constexpr uint64_t kObjectReleaseRva = 0x41db90, kPoolReleaseRva = 0x41dbe0, kCpuViewRva = 0x9e1dac0,
                   kPoolSizeRva = 0x9e1dac8, kGpuViewRva = 0x9e1dad0;

void checked_object_release(uint8_t* object) {
  uint64_t& gpu = *reinterpret_cast<uint64_t*>(object + 0x18);
  if (!gpu) return;
  const uint64_t address = gpu & ~0xFFFFull;
  const uint32_t size = static_cast<uint32_t>(
      ((*reinterpret_cast<uint16_t*>(object + 0xC) & 0xFFFu) * *reinterpret_cast<uint32_t*>(object + 0x8) + 0xFFFFu) &
      0xFFFF0000u);
  const uint64_t cpu_view = *reinterpret_cast<uint64_t*>(g_title_base + kCpuViewRva);
  const uint64_t gpu_view = *reinterpret_cast<uint64_t*>(g_title_base + kGpuViewRva);
  static std::atomic<bool> bases{false};
  if (!bases.exchange(true)) {
    logf("pages: the pool's CPU view 0x%llx, GPU view 0x%llx, %llu bytes", static_cast<unsigned long long>(cpu_view),
         static_cast<unsigned long long>(gpu_view),
         static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(g_title_base + kPoolSizeRva)));
  }
  const uint64_t pool = address + cpu_view - gpu_view;
  const uint64_t* const table = *reinterpret_cast<uint64_t**>(g_title_base + kPageTableRva);
  const uint64_t pool_size = *reinterpret_cast<uint64_t*>(g_title_base + kPoolSizeRva);
  if (pool >= cpu_view && pool < cpu_view + pool_size && size) {
    const uint32_t unit = static_cast<uint32_t>((pool - cpu_view) >> 16), units = size >> 16;
    const uint32_t size_class = static_cast<uint32_t>(table[unit] >> 15) & 0x1F;
    const uint32_t block = size_class >= 0x18 ? 1u : (1u << size_class);
    static std::atomic<int> all{0};
    if (all.fetch_add(1) < 60) {  // the first releases, good or not, to compare the objects
      const uint64_t* q = reinterpret_cast<const uint64_t*>(object);
      logf("pages: release of %u units from unit 0x%x (block of %u, class %u, count %llu); object words %llx %llx %llx %llx "
           "%llx %llx %llx %llx",
           units, unit, block, size_class, static_cast<unsigned long long>(table[unit] & 0x7FFF),
           static_cast<unsigned long long>(q[0]), static_cast<unsigned long long>(q[1]), static_cast<unsigned long long>(q[2]),
           static_cast<unsigned long long>(q[3]), static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]),
           static_cast<unsigned long long>(q[6]), static_cast<unsigned long long>(q[7]));
    }
    if (units > block && (size_class == 0 || size_class >= 0x18)) {
      static std::atomic<int> told{0};
      if (told.fetch_add(1) < 30) {
        void* frames[6] = {};
        const USHORT got = RtlCaptureStackBackTrace(1, 6, frames, nullptr);
        std::string text;
        for (USHORT k = 0; k < got; ++k) text += " " + g_process->describe_address(reinterpret_cast<uint64_t>(frames[k]));
        const uint64_t* q = reinterpret_cast<const uint64_t*>(object);
        logf("pages: object %p%s releases %u units from unit 0x%x, a block of %u (entry 0x%llx); object words %llx %llx %llx "
             "%llx %llx %llx;%s",
             object, d3d12x::is_object(object) ? " (one of ours)" : "", units, unit, block,
             static_cast<unsigned long long>(table[unit]), static_cast<unsigned long long>(q[0]),
             static_cast<unsigned long long>(q[1]), static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]),
             static_cast<unsigned long long>(q[4]), static_cast<unsigned long long>(q[5]), text.c_str());
        // +0x20: one of our resources?
        D3D12_RESOURCE_DESC rd{};
        uint64_t ra = 0;
        const void* r = reinterpret_cast<const void*>(q[4]);
        if (d3d12x::is_object(r) && d3d12x::resource_memory(r, &rd, &ra)) {
          const D3D12_RESOURCE_ALLOCATION_INFO ai = d3d12x::allocation_info(rd);
          logf("pages:   +0x20 is our resource %p: dim %u, %llux%u x%u, %u mips, format %u, layout 0x%x, flags 0x%x, at 0x%llx; "
               "our size %llu", r, rd.Dimension, static_cast<unsigned long long>(rd.Width), rd.Height, rd.DepthOrArraySize,
               rd.MipLevels, rd.Format, static_cast<uint32_t>(rd.Layout), rd.Flags, static_cast<unsigned long long>(ra),
               static_cast<unsigned long long>(ai.SizeInBytes));
        } else if (d3d12x::is_object(r)) {
          logf("pages:   +0x20 is one of our objects (%p), not a resource", r);
        }
      }
    }
  }
  reinterpret_cast<void (*)(uint64_t, uint64_t)>(g_title_base + kPoolReleaseRva)(address, uint64_t{size});
  gpu = 0;
}

// AMERICAS_WATCH_TEXTURES=<width>x<height> (diagnosis): after the title's texture init
// (game+0x164db0: texture, description, two flags, then a byte, a u32 and a pointer on the stack)
// makes a texture of that size, its arguments are logged and its address field (+0x18) gets a
// hardware watch (up to four). The init's first 14 bytes (pushes and the frame) run in a stub
// that jumps back.
constexpr uint64_t kTextureInitRva = 0x164db0;
uint32_t g_watch_width = 0, g_watch_height = 0;
using TextureInit = uint64_t (*)(uint8_t*, const uint8_t*, uint64_t, uint64_t, uint64_t, uint64_t, const uint8_t*);
TextureInit g_texture_init = nullptr;

uint64_t watched_texture_init(uint8_t* texture, const uint8_t* desc, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                              const uint8_t* a7) {
  const uint64_t result = g_texture_init(texture, desc, a3, a4, a5, a6, a7);
  const uint32_t width = *reinterpret_cast<uint32_t*>(texture + 0x8);
  const uint32_t height = *reinterpret_cast<uint16_t*>(texture + 0xC) & 0xFFFu;
  static std::atomic<int> seen{0};
  if (seen.fetch_add(1) < 12) {
    logf("pages: texture init %p: +0x8 %u, +0xc 0x%x, address 0x%llx", texture, width,
         *reinterpret_cast<uint16_t*>(texture + 0xC), static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(texture + 0x18)));
  }
  if (width == g_watch_width && height == g_watch_height) {
    void* frames[6] = {};
    const USHORT got = RtlCaptureStackBackTrace(0, 6, frames, nullptr);
    std::string text;
    for (USHORT k = 0; k < got; ++k) text += " " + g_process->describe_address(reinterpret_cast<uint64_t>(frames[k]));
    const uint64_t* d = reinterpret_cast<const uint64_t*>(desc);
    const uint64_t* m = reinterpret_cast<const uint64_t*>(a7);
    logf("pages: texture %p made %ux%u (flags 0x%llx 0x%llx, 0x%llx, 0x%llx), address 0x%llx; description %llx %llx %llx; "
         "memory %llx %llx;%s",
         texture, width, height, static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
         static_cast<unsigned long long>(a5 & 0xFF), static_cast<unsigned long long>(a6 & 0xFFFFFFFF),
         static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(texture + 0x18)), static_cast<unsigned long long>(d[0]),
         static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[2]),
         static_cast<unsigned long long>(m ? m[0] : 0), static_cast<unsigned long long>(m ? m[1] : 0), text.c_str());
    add_hw_watch(reinterpret_cast<uint64_t>(texture + 0x18), 8);
  }
  return result;
}

// The title's watchdog thread (+0x2215a80) ends the run (+0x23cf620 writes to address 0) when
// its main loop gives no heartbeat for 30 one-second ticks. On a PC the first run of a scene
// compiles every new pipeline in the driver, on the title's own threads, and that alone passed
// 30 s. The tick budget (nine `mov reg, 30` loads) becomes AMERICAS_TITLE_WATCHDOG_S seconds
// (600 by default; 30 keeps the title's). Patched in memory only when every site holds the
// expected bytes.
void install_watchdog_patience(uint8_t* base, size_t size) {
  char text[16] = {};
  const uint32_t seconds =
      GetEnvironmentVariableA("AMERICAS_TITLE_WATCHDOG_S", text, sizeof text) ? static_cast<uint32_t>(std::strtoul(text, nullptr, 10)) : 600u;
  if (seconds <= 30 || size < 0x2215d80) return;  // not this title (the test fixture is small)
  struct Site {
    uint32_t rva;
    uint8_t opcode;  // mov ebp / ebx / eax, imm32
  };
  static const Site kSites[] = {{0x2215acc, 0xBD}, {0x2215b3a, 0xBD}, {0x2215b62, 0xBD}, {0x2215bfd, 0xBD}, {0x2215c64, 0xBB},
                                {0x2215cde, 0xBD}, {0x2215d06, 0xBD}, {0x2215d67, 0xBD}, {0x2215d78, 0xB8}};
  for (const Site& s : kSites) {
    const uint8_t expected[] = {s.opcode, 0x1E, 0, 0, 0};
    if (std::memcmp(base + s.rva, expected, sizeof expected) != 0) {
      logf("watchdog: the title's watchdog is not where this build expects it; left at 30 s");
      return;
    }
  }
  for (const Site& s : kSites) {
    uint8_t* at = base + s.rva + 1;
    DWORD old = 0;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, &seconds, 4);
    VirtualProtect(at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
  }
  logf("watchdog: the title's main-loop watchdog waits %u s instead of 30 (AMERICAS_TITLE_WATCHDOG_S)", seconds);
}

void install_page_release_clamp(uint8_t* base) {
  g_title_base = base;
  char size[32] = {};
  if (GetEnvironmentVariableA("AMERICAS_WATCH_TEXTURES", size, sizeof size) &&
      sscanf_s(size, "%ux%u", &g_watch_width, &g_watch_height) == 2) {
    uint8_t* at = base + kTextureInitRva;
    // push r15; push r14; push r12; push rsi; push rdi; push rbp; push rbx; sub rsp, 0x50
    const uint8_t expected[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x50};
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (stub && std::memcmp(at, expected, sizeof expected) == 0) {
      // The stub: the 14 bytes, then jmp [rip] to the init's 15th byte.
      std::memcpy(stub, expected, sizeof expected);
      const uint8_t far_jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
      std::memcpy(stub + sizeof expected, far_jump, sizeof far_jump);
      const uint64_t back = reinterpret_cast<uint64_t>(at + sizeof expected);
      std::memcpy(stub + sizeof expected + sizeof far_jump, &back, 8);
      FlushInstructionCache(GetCurrentProcess(), stub, 64);
      g_texture_init = reinterpret_cast<TextureInit>(stub);
      uint8_t jump[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
      const uint64_t target = reinterpret_cast<uint64_t>(&watched_texture_init);
      std::memcpy(jump + 2, &target, 8);
      DWORD old = 0;
      if (VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(at, jump, sizeof jump);
        VirtualProtect(at, sizeof jump, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
        logf("pages: AMERICAS_WATCH_TEXTURES: watching %ux%u textures", g_watch_width, g_watch_height);
      }
    } else {
      logf("pages: AMERICAS_WATCH_TEXTURES: the texture init is not where this build expects it");
    }
  }
  if (GetEnvironmentVariableA("AMERICAS_CHECK_PAGE_RELEASE", nullptr, 0)) {
    uint8_t* at = base + kObjectReleaseRva;
    const uint8_t expected[] = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCE, 0x48, 0x8B, 0x49, 0x18};
    if (std::memcmp(at, expected, sizeof expected) == 0) {
      uint8_t jump[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
      const uint64_t target = reinterpret_cast<uint64_t>(&checked_object_release);
      std::memcpy(jump + 2, &target, 8);
      DWORD old = 0;
      if (VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(at, jump, sizeof jump);
        VirtualProtect(at, sizeof jump, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
        logf("pages: AMERICAS_CHECK_PAGE_RELEASE: object page releases are checked");
      }
    }
  }
  if (!GetEnvironmentVariableA("AMERICAS_CLAMP_PAGE_RELEASE", nullptr, 0)) return;
  uint8_t* at = base + kPageReleaseRva;
  // mov rdx, [rip+...] (the page table); mov eax, ecx; mov ecx, ecx: the version this was read from.
  const uint8_t expected[] = {0x48, 0x8B, 0x15, 0x71, 0x72, 0x0D, 0x07, 0x89, 0xC8, 0x89, 0xC9};
  if (std::memcmp(at, expected, sizeof expected) != 0) {
    logf("pages: AMERICAS_CLAMP_PAGE_RELEASE: the release is not where this build expects it; not patched");
    return;
  }
  uint8_t jump[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax, imm64; jmp rax
  const uint64_t target = reinterpret_cast<uint64_t>(&clamped_page_release);
  std::memcpy(jump + 2, &target, 8);
  DWORD old = 0;
  if (!VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) return;
  std::memcpy(at, jump, sizeof jump);
  VirtualProtect(at, sizeof jump, old, &old);
  FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
  logf("pages: AMERICAS_CLAMP_PAGE_RELEASE: releases of pages with no reference left are refused");
}

void ensure_hw_watcher() {
  static std::once_flag once;
  std::call_once(once, [] {
    AddVectoredExceptionHandler(1, &on_hw_watch);
    std::thread(hw_watch_thread).detach();
  });
}

void start_hw_watch() {
  char spec[256] = {};
  if (!GetEnvironmentVariableA("AMERICAS_HW_WATCH", spec, sizeof spec)) return;
  for (const char* p = spec; *p && g_hw_watch_count < 4;) {
    char* end = nullptr;
    HwWatch w;
    w.address = std::strtoull(p, &end, 16);
    if (end == p) break;
    if (*end == ':') w.length = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
    if (w.length != 1 && w.length != 2 && w.length != 4) w.length = 8;
    w.address &= ~static_cast<uint64_t>(w.length - 1);  // breakpoints are aligned to their length
    g_hw_watches[g_hw_watch_count++] = w;
    logf("hw watch: watching writes to 0x%llx (%u bytes)", static_cast<unsigned long long>(w.address), w.length);
    p = *end == ',' ? end + 1 : end;
    if (!*end) break;
  }
  if (!g_hw_watch_count) return;
  ensure_hw_watcher();
}

// A watch added while the title runs (diagnosis code that finds what to watch); false when
// all four are taken.
bool add_hw_watch(uint64_t address, uint32_t length) {
  static std::mutex lock;
  std::lock_guard g(lock);
  if (g_hw_watch_count >= 4) return false;
  HwWatch w;
  w.length = length;
  w.address = address & ~static_cast<uint64_t>(length - 1);
  g_hw_watches[g_hw_watch_count] = w;
  ++g_hw_watch_count;
  ++g_hw_generation;
  logf("hw watch: watching writes to 0x%llx (%u bytes), added", static_cast<unsigned long long>(w.address), w.length);
  ensure_hw_watcher();
  return true;
}

}  // namespace

TitleProcess* process() { return g_process; }

TitleProcess::TitleProcess(RunOptions options) : options_(std::move(options)), stubs_(options_.unimplemented) {
  g_process = this;
}

// ---- command line ----

void TitleProcess::build_command_line() {
  args_w_.clear();
  args_w_.push_back(options_.title.wstring());
  for (const std::wstring& a : options_.args) args_w_.push_back(a);
  command_line_w_.clear();
  for (size_t i = 0; i < args_w_.size(); ++i) {
    if (i) command_line_w_ += L' ';
    command_line_w_ += quote_arg(args_w_[i]);
    if (i == 0) winmain_offset_w_ = command_line_w_.size() + (args_w_.size() > 1 ? 1 : 0);
  }
  command_line_a_ = narrow(command_line_w_);
  winmain_offset_a_ = narrow(command_line_w_.substr(0, winmain_offset_w_)).size();

  args_a_.clear();
  for (const std::wstring& a : args_w_) args_a_.push_back(narrow(a));
  argv_storage_.clear();
  wargv_storage_.clear();
  for (std::string& a : args_a_) argv_storage_.push_back(a.data());
  for (std::wstring& a : args_w_) wargv_storage_.push_back(a.data());
  argv_storage_.push_back(nullptr);
  wargv_storage_.push_back(nullptr);
  argc_ = static_cast<int>(args_a_.size());
  argv_ = argv_storage_.data();
  wargv_ = wargv_storage_.data();
}

// ---- loading ----

bool TitleProcess::load(std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::filesystem::path dir = options_.title.parent_path();
  std::vector<std::string> title_dlls;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::wstring ext = entry.path().extension().wstring();
    if (entry.is_regular_file(ec) && CompareStringOrdinal(ext.c_str(), -1, L".dll", -1, TRUE) == CSTR_EQUAL) {
      title_dlls.push_back(narrow(entry.path().filename().wstring()));
    }
  }
  policy_ = loader::ImportPolicy(title_dlls);
  build_command_line();
  SetUnhandledExceptionFilter(&top_level_filter);
  start_hw_watch();
  std::signal(SIGABRT, &on_abort);
  install_cpu_emulation();
  set_title_code_test([](const void* address) { return g_process && g_process->module_containing(address) != nullptr; });
  if (!tls_.discover(error)) return false;
  // Host DLLs (a host C++ runtime, say) asking which image a title address belongs to get our answer.
  if (const hle::Function* f = registry_.find_wrapper("RtlPcToFileHeader")) {
    rtl_pc_to_file_header_ = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlPcToFileHeader"));
    rtl_pc_wrapper_ = f->address;
    logf("host import tables redirected for RtlPcToFileHeader: %zu", patch_loaded_imports(rtl_pc_to_file_header_, rtl_pc_wrapper_));
  }

  MapOptions map_options;
  map_options.force_relocation = options_.force_relocation;
  if (options_.image_window) {
    VirtualFree(options_.image_window, 0, MEM_RELEASE);
    options_.image_window = nullptr;
  } else {
    logf("the launcher could not hold 0x%s: the executable may have to move", hex(kImageWindow).c_str());
  }
  auto exe = map_image(options_.title, map_options, error);
  if (!exe) return false;
  if (exe->info.is_dll()) {
    error = narrow(options_.title.wstring()) + " is a DLL, not an executable";
    return false;
  }
  modules_.push_back(std::make_unique<MappedImage>(std::move(*exe)));
  exe_ = modules_.back().get();
  publish(exe_);
  logf("mapped %s at %p%s (%s)", exe_->name.c_str(), static_cast<void*>(exe_->base), exe_->relocated ? ", relocated" : "",
       format_bytes(exe_->size).c_str());
  if (!bind(*exe_, error)) return false;
  if (!prepare(*exe_, true, error)) return false;
  for (MappedImage* dep : deps_[exe_]) {
    if (!initialize(*dep, error)) return false;
  }
  return true;
}

MappedImage* TitleProcess::require_module(std::string_view name, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (MappedImage* m = find_module(name)) return m;
  if (!policy_.is_title_module(normalize_module_name(name))) return nullptr;

  std::filesystem::path path;
  const std::string wanted = normalize_module_name(name);
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(options_.title.parent_path(), ec)) {
    if (to_lower_ascii(narrow(entry.path().filename().wstring())) == wanted) path = entry.path();
  }
  if (path.empty()) {
    error = "title module " + wanted + " not found";
    return nullptr;
  }
  MapOptions map_options;
  map_options.force_relocation = options_.force_relocation;
  auto mapped = map_image(path, map_options, error);
  if (!mapped) return nullptr;
  if (!mapped->info.is_dll()) {
    error = mapped->name + " is not a DLL";
    return nullptr;
  }
  modules_.push_back(std::make_unique<MappedImage>(std::move(*mapped)));
  MappedImage* m = modules_.back().get();
  publish(m);
  logf("mapped %s at %p%s (%s)", m->name.c_str(), static_cast<void*>(m->base), m->relocated ? ", relocated" : "",
       format_bytes(m->size).c_str());
  if (!bind(*m, error) || !prepare(*m, false, error)) {
    error = m->name + ": " + error;
    return nullptr;
  }
  return m;
}

MappedImage* TitleProcess::load_module(std::string_view name, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  MappedImage* m = require_module(name, error);
  if (!m) return nullptr;
  if (!initialize(*m, error)) return nullptr;
  return m;
}

bool TitleProcess::bind(MappedImage& m, std::string& error) {
  for (const pe::ImportedModule& im : m.info.imports) {
    if (!im.iat_rva) continue;
    auto* iat = m.at<uint64_t>(im.iat_rva);
    if (!m.contains(iat) || !m.contains(iat + im.functions.size())) {
      error = "import table of " + im.dll + " lies outside the image";
      return false;
    }
    for (size_t k = 0; k < im.functions.size(); ++k) {
      const pe::ImportedFunction& f = im.functions[k];
      void* address = resolve_import(im.dll, f.name, f.ordinal, f.by_ordinal);
      if (!address) {
        error = "cannot bind " + im.dll + "!" + (f.by_ordinal ? "#" + std::to_string(f.ordinal) : f.name);
        return false;
      }
      iat[k] = reinterpret_cast<uint64_t>(address);
    }
    if (im.delay_load && im.module_handle_rva) {
      // Pre-bound: give the delay-load helper a handle so it never runs for this module.
      HMODULE handle = nullptr;
      if (MappedImage* t = find_module(im.dll)) handle = reinterpret_cast<HMODULE>(t->base);
      else if (!loader::is_console_only_module(im.dll)) handle = host_module(im.dll);
      *m.at<uint64_t>(im.module_handle_rva) = reinterpret_cast<uint64_t>(handle);
    }
    if (MappedImage* t = find_module(im.dll); t && t != &m) deps_[&m].push_back(t);
  }
  return true;
}

bool TitleProcess::prepare(MappedImage& m, bool is_executable, std::string& error) {
  CpuCompatResult cpu;
  if (!apply_cpu_compat(m, options_.cache_dir, options_.allow_missing_cpu_features, cpu, error)) return false;
  cpu_[&m] = cpu;
  if (!tls_.install(m, is_executable, error) || !finish_image(m, error)) return false;
  if (is_executable) {
    install_page_release_clamp(m.base);
    install_watchdog_patience(m.base, m.size);
  }
  return true;
}

void TitleProcess::call_tls_callbacks(const MappedImage& m, unsigned long reason) {
  if (!m.info.tls) return;
  const int64_t delta = reinterpret_cast<int64_t>(m.base) - static_cast<int64_t>(m.info.image_base);
  for (uint64_t va : m.info.tls->callbacks) {
    auto cb = reinterpret_cast<PIMAGE_TLS_CALLBACK>(static_cast<int64_t>(va) + delta);
    cb(m.base, reason, nullptr);
  }
}

bool TitleProcess::initialize(MappedImage& m, std::string& error) {
  if (m.initialized || &m == exe_) return true;
  m.initialized = true;  // set first: dependency cycles end here
  for (MappedImage* dep : deps_[&m]) {
    if (!initialize(*dep, error)) return false;
  }
  call_tls_callbacks(m, DLL_PROCESS_ATTACH);
  if (m.info.entry_rva) {
    using DllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    auto entry = reinterpret_cast<DllMain>(m.base + m.info.entry_rva);
    if (!entry(reinterpret_cast<HINSTANCE>(m.base), DLL_PROCESS_ATTACH, nullptr)) {
      error = m.name + ": DllMain refused to initialize";
      return false;
    }
  }
  mark_ready(&m);
  logf("initialized %s", m.name.c_str());
  return true;
}

int TitleProcess::run() {
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    loaded_ = true;
  }
  call_tls_callbacks(*exe_, DLL_PROCESS_ATTACH);
  exe_->initialized = true;
  mark_ready(exe_);
  using Entry = int(__cdecl*)(void*);
  auto entry = reinterpret_cast<Entry>(exe_->base + exe_->info.entry_rva);
  logf("starting %s", exe_->name.c_str());
  return entry(reinterpret_cast<void*>(__readgsqword(0x60)));  // the entry point gets the PEB, as from Windows
}

void TitleProcess::on_thread(unsigned long reason) {
  const size_t n = image_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (!ready_[i].load(std::memory_order_acquire)) continue;
    MappedImage* m = images_[i].load(std::memory_order_acquire);
    call_tls_callbacks(*m, reason);
    if (m != exe_ && m->thread_calls && m->info.entry_rva) {
      using DllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
      reinterpret_cast<DllMain>(m->base + m->info.entry_rva)(reinterpret_cast<HINSTANCE>(m->base), reason, nullptr);
    }
  }
}

// ---- lookups ----

MappedImage* TitleProcess::find_module(std::string_view name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string wanted = normalize_module_name(name);
  for (const auto& m : modules_) {
    if (to_lower_ascii(m->name) == wanted) return m.get();
  }
  return nullptr;
}

MappedImage* TitleProcess::module_containing(const void* address) {
  const size_t n = image_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    MappedImage* m = images_[i].load(std::memory_order_acquire);
    if (m && m->contains(address)) return m;
  }
  return nullptr;
}

void TitleProcess::mark_ready(const MappedImage* m) {
  const size_t n = image_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (images_[i].load(std::memory_order_acquire) == m) ready_[i].store(true, std::memory_order_release);
  }
}

void TitleProcess::publish(MappedImage* m) {
  const size_t n = image_count_.load(std::memory_order_relaxed);
  if (n >= kMaxImages) return;
  images_[n].store(m, std::memory_order_release);
  image_count_.store(n + 1, std::memory_order_release);
}

HMODULE TitleProcess::pseudo_module(std::string_view name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string n = normalize_module_name(name);
  if (!loader::is_console_only_module(n)) return nullptr;
  for (const auto& p : pseudo_names_) {
    if (*p == n) return reinterpret_cast<HMODULE>(p.get());
  }
  pseudo_names_.push_back(std::make_unique<std::string>(n));
  return reinterpret_cast<HMODULE>(pseudo_names_.back().get());
}

const std::string* TitleProcess::pseudo_module_name(HMODULE handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& p : pseudo_names_) {
    if (reinterpret_cast<HMODULE>(p.get()) == handle) return p.get();
  }
  return nullptr;
}

HMODULE TitleProcess::host_module(const std::string& dll) {
  const std::string key = to_lower_ascii(dll);
  auto it = host_modules_.find(key);
  if (it != host_modules_.end()) return it->second;
  HMODULE h = LoadLibraryExW(widen(dll).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  host_modules_[key] = h;
  if (h && rtl_pc_wrapper_) patch_module_imports(h, rtl_pc_to_file_header_, rtl_pc_wrapper_);
  return h;
}

std::string TitleProcess::host_ordinal_name(const std::string& dll, uint16_t ordinal) {
  const std::string key = to_lower_ascii(dll);
  auto [it, inserted] = host_ordinals_.try_emplace(key);
  if (inserted) {
    if (HMODULE h = host_module(dll)) {
      const auto* base = reinterpret_cast<const uint8_t*>(h);
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
      const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
      if (d.VirtualAddress) {
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress);
        const auto* names = reinterpret_cast<const uint32_t*>(base + ed->AddressOfNames);
        const auto* ords = reinterpret_cast<const uint16_t*>(base + ed->AddressOfNameOrdinals);
        for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
          it->second[static_cast<uint16_t>(ed->Base + ords[i])] = reinterpret_cast<const char*>(base + names[i]);
        }
      }
    }
  }
  const auto found = it->second.find(ordinal);
  return found == it->second.end() ? std::string() : found->second;
}

void* TitleProcess::resolve_import(std::string_view dll_view, std::string_view function, uint16_t ordinal,
                                   bool by_ordinal) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string dll(dll_view);
  std::string fn(function);
  const bool console = loader::is_console_only_module(dll);
  const bool title = policy_.is_title_module(dll);
  if (by_ordinal && fn.empty() && !console && !title && loader::is_host_module(dll)) fn = host_ordinal_name(dll, ordinal);
  const std::string label = fn.empty() ? "#" + std::to_string(ordinal) : fn;

  switch (policy_.route(dll, fn)) {
    case loader::Route::Ours:
      // By name, or as "#<ordinal>" for functions a console module exports by ordinal only.
      if (const hle::Function* f = registry_.find(dll, fn.empty() ? label : fn)) return f->address;
      return stubs_.stub(dll, label, "console function not implemented yet");
    case loader::Route::Wrap:
      if (const hle::Function* f = registry_.find_wrapper(fn)) return f->address;
      return stubs_.stub(dll, label, "needs console behaviour we do not provide yet; refused");
    case loader::Route::Host:
      // Diagnosis hooks over host functions (AMERICAS_TRACE_MALLOC) are registered by module.
      if (const hle::Function* f = fn.empty() ? nullptr : registry_.find(dll, fn)) return f->address;
      if (HMODULE h = host_module(dll)) {
        const FARPROC p = (by_ordinal && fn.empty()) ? GetProcAddress(h, MAKEINTRESOURCEA(ordinal))
                                                     : GetProcAddress(h, fn.c_str());
        if (p) return reinterpret_cast<void*>(p);
      }
      return stubs_.stub(dll, label, "not on this PC");
    case loader::Route::Title: {
      std::string error;
      MappedImage* t = loaded_ ? load_module(dll, error) : require_module(dll, error);
      if (!t) return stubs_.stub(dll, label, "title module did not load: " + error);
      const MappedImage::Export e = by_ordinal ? t->find_export_ordinal(ordinal) : t->find_export(fn);
      if (void* const* p = std::get_if<void*>(&e)) return *p;
      if (const std::string* fwd = std::get_if<std::string>(&e)) {
        const size_t dot = fwd->find('.');
        if (dot != std::string::npos) {
          const std::string mod = fwd->substr(0, dot) + ".dll";
          const std::string target = fwd->substr(dot + 1);
          if (!target.empty() && target[0] == '#') {
            return resolve_import(mod, "", static_cast<uint16_t>(std::atoi(target.c_str() + 1)), true);
          }
          return resolve_import(mod, target, 0, false);
        }
      }
      return stubs_.stub(dll, label, "the title module does not export it");
    }
    case loader::Route::Unresolved:
      break;
  }
  return stubs_.stub(dll, label, "nothing provides it");
}

// ---- crashes ----

LPTOP_LEVEL_EXCEPTION_FILTER TitleProcess::set_title_crash_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  LPTOP_LEVEL_EXCEPTION_FILTER old = title_filter_;
  title_filter_ = filter;
  return old;
}

std::string TitleProcess::describe_address(uint64_t address) {
  if (const MappedImage* m = module_containing(reinterpret_cast<const void*>(address))) {
    return m->name + "+0x" + hex(address - reinterpret_cast<uint64_t>(m->base));
  }
  HMODULE h = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &h)) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(h, path, MAX_PATH);
    return narrow(std::filesystem::path(path).filename().wstring()) + "+0x" + hex(address - reinterpret_cast<uint64_t>(h));
  }
  return "0x" + hex(address);
}

// A thread's frames from `ctx` on, as "\n  stack:" and one line a frame.
std::string TitleProcess::describe_stack(CONTEXT ctx) {
  std::string text = "\n  stack:";
  for (int frame = 0; frame < 32 && ctx.Rip; ++frame) {
    text += "\n    " + describe_address(ctx.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (fn) {
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    } else {
      if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
    }
  }
  return text;
}

// abort() in the host's C runtime (std::terminate on an escaped exception lands there too): the
// exception's message, if there is one, and the aborting thread's stack, to the log and
// crash.txt. The process then ends as abort() ends it.
void on_abort(int) {
  if (!g_process || InterlockedIncrement(&g_process->crashing_) != 1) return;
  std::string text = "crash: abort() in the host's C runtime";
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      text += std::string(", an exception escaped: ") + x.what();
    } catch (...) {
      text += ", an exception of unknown type escaped";
    }
  }
  CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  text += g_process->describe_stack(ctx);
  log_line(text);
  std::ofstream(g_process->options_.logs_dir / L"crash.txt", std::ios::binary) << text << '\n';
  g_process->write_report();
}

// The class of the object at `v`, from the MSVC run-time type information its vtable points
// to (x64: the complete object locator before the vtable, signature 1, holding its own RVA);
// empty when `v` is not such an object.
static bool rtti_name_at(uint64_t v, char* out, size_t size) {
  __try {
    const uint64_t vtable = *reinterpret_cast<const uint64_t*>(v);
    const uint64_t col = *reinterpret_cast<const uint64_t*>(vtable - 8);
    const auto* c = reinterpret_cast<const uint32_t*>(col);
    if (c[0] != 1) return false;
    const uint64_t base = col - c[5];
    const char* name = reinterpret_cast<const char*>(base + c[3] + 16);
    if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return false;
    size_t n = 0;
    for (; n + 1 < size && name[n] && n < 160; ++n) out[n] = name[n];
    out[n] = 0;
    return n > 3;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static std::string describe_objects(const CONTEXT& ctx) {
  const struct {
    const char* name;
    uint64_t value;
  } regs[] = {{"rax", ctx.Rax}, {"rbx", ctx.Rbx}, {"rcx", ctx.Rcx}, {"rdx", ctx.Rdx}, {"rsi", ctx.Rsi}, {"rdi", ctx.Rdi},
              {"rbp", ctx.Rbp}, {"r8", ctx.R8},   {"r9", ctx.R9},   {"r12", ctx.R12}, {"r13", ctx.R13}, {"r14", ctx.R14},
              {"r15", ctx.R15}};
  std::string text;
  for (const auto& r : regs) {
    char name[192];
    if (r.value > 0x10000 && rtti_name_at(r.value, name, sizeof name)) text += std::string("\n  ") + r.name + " is a " + name;
  }
  return text;
}

long TitleProcess::crash_filter(EXCEPTION_POINTERS* info) {
  if (InterlockedIncrement(&crashing_) == 1) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    CONTEXT ctx = *info->ContextRecord;
    std::string text = "crash: exception 0x" + hex(r->ExceptionCode, 8) + " at " +
                       describe_address(reinterpret_cast<uint64_t>(r->ExceptionAddress));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
      text += r->ExceptionInformation[0] == 0 ? ", reading 0x" : r->ExceptionInformation[0] == 1 ? ", writing 0x" : ", executing 0x";
      text += hex(r->ExceptionInformation[1]);
    }
    if (r->ExceptionCode == kUnimplementedException && r->NumberParameters >= 1) {
      const auto* stub = reinterpret_cast<const StubInfo*>(r->ExceptionInformation[0]);
      text += ": unimplemented " + stub->dll + "!" + stub->function;
    }
    char regs[512];
    std::snprintf(regs, sizeof regs,
                  "\n  rax %016llx rbx %016llx rcx %016llx rdx %016llx\n  rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n"
                  "  r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n  r12 %016llx r13 %016llx r14 %016llx r15 %016llx",
                  ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp, ctx.R8, ctx.R9, ctx.R10,
                  ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);
    text += regs;
    text += describe_objects(ctx);
    text += describe_stack(ctx);
    log_line(text);
    std::ofstream(options_.logs_dir / L"crash.txt", std::ios::binary) << text << '\n';
    write_report();
    // Every other thread's stack too (hang.txt): the title's own fatals (its watchdog) are about
    // what another thread is stuck on.
    dump_threads();
  }
  if (title_filter_) return title_filter_(info);
  return EXCEPTION_EXECUTE_HANDLER;
}

void TitleProcess::dump_threads() {
  struct Captured {
    DWORD id;
    uint64_t frames[40];
    int count;
    CONTEXT top;  // the registers where it stopped
  };
  std::vector<DWORD> ids;
  const DWORD self = GetCurrentThreadId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te)) {
      if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
    }
    CloseHandle(snapshot);
  }
  std::vector<Captured> captured(ids.size());
  std::string text = "threads: " + std::to_string(ids.size()) + " besides the reporting one\n";
  for (size_t i = 0; i < ids.size(); ++i) {
    Captured& c = captured[i];
    c.id = ids[i];
    c.count = 0;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, c.id);
    if (!t) continue;
    // Nothing that allocates while the thread is stopped: it may hold the heap's lock.
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &ctx)) {
        c.top = ctx;
        for (; c.count < 40 && ctx.Rip; ++c.count) {
          c.frames[c.count] = ctx.Rip;
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
          if (!fn) {
            if (c.count) break;
            // A leaf function (a system call stub): its return address is on top of the stack.
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
          }
          void* handler_data = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
        }
      }
      ResumeThread(t);
    }
    PWSTR name = nullptr;
    std::string label;
    if (SUCCEEDED(GetThreadDescription(t, &name)) && name) {
      label = narrow(name);
      LocalFree(name);
    }
    CloseHandle(t);
    text += "\nthread " + std::to_string(c.id) + (label.empty() ? "" : " \"" + label + "\"") + ":";
    for (int k = 0; k < c.count; ++k) text += "\n    " + describe_address(c.frames[k]);
    // A thread running outside the system (not waiting): its registers too (a spinning loop's
    // state names what it spins on).
    if (c.count && describe_address(c.frames[0]).find("ntdll") == std::string::npos &&
        describe_address(c.frames[0]).find("win32u") == std::string::npos) {
      const CONTEXT& r = c.top;
      const std::pair<const char*, uint64_t> regs[] = {{"rax", r.Rax}, {"rbx", r.Rbx}, {"rcx", r.Rcx}, {"rdx", r.Rdx},
                                                       {"rsi", r.Rsi}, {"rdi", r.Rdi}, {"rbp", r.Rbp}, {"rsp", r.Rsp},
                                                       {"r8", r.R8},   {"r9", r.R9},   {"r10", r.R10}, {"r11", r.R11},
                                                       {"r12", r.R12}, {"r13", r.R13}, {"r14", r.R14}, {"r15", r.R15}};
      text += "\n  registers:";
      for (const auto& [reg, value] : regs) text += std::string(" ") + reg + " " + hex(value);
    }
  }
  std::ofstream(options_.logs_dir / L"hang.txt", std::ios::binary) << text << '\n';
  logf("hang.txt: stacks of %zu threads", ids.size());
}

void TitleProcess::write_report() const {
  JsonWriter j;
  j.begin_object();
  j.field("title", narrow(options_.title.wstring()));
  j.key("modules").begin_array();
  for (const auto& m : modules_) {
    j.begin_object().field("name", m->name).field("base", "0x" + hex(reinterpret_cast<uint64_t>(m->base)))
        .field("relocated", m->relocated).field("initialized", m->initialized).end_object();
  }
  j.end_array();
  j.key("stubs").begin_array();
  for (const StubInfo* s : stubs_.all()) {
    j.begin_object().field("import", s->dll + "!" + s->function).field("why", s->why).field("calls", s->calls).end_object();
  }
  j.end_array();
  j.key("cpu").begin_object();
  for (const auto& [m, r] : cpu_) {
    j.key(m->name).begin_object().field("patched", r.patched).field("emulated_sites", r.emulated)
        .field("census_from_cache", r.from_cache).end_object();
  }
  j.key("emulation_hits").begin_array();
  for (const EmulationHit& h : emulation_hits()) {
    j.begin_object().field("at", const_cast<TitleProcess*>(this)->describe_address(h.address)).field("count", h.count).end_object();
  }
  j.end_array();
  j.end_object();
  const mem::MemoryStats ms = mem::title_memory().stats();
  j.key("memory").begin_object();
  j.field("console", std::string(current_console().name)).field("physical_capacity", ms.physical_capacity)
      .field("physical_allocated", ms.physical_allocated).field("physical_peak", ms.physical_peak)
      .field("physical_committed", ms.physical_committed).field("views", ms.views).field("map_calls", ms.map_calls)
      .field("reservations", ms.reservations).field("regions", ms.regions).field("refused", ms.refused);
  j.end_object();
  j.key("dstorage");
  storage::write_storage_report(j);
  j.key("runtime");
  gdk::write_runtime_report(j);
  d3d12x::write_graphics_report(j);
  j.key("passthrough").begin_array();
  for (const hle::Function& f : registry_.all()) {
    if (f.passthrough) j.value(f.name);
  }
  j.end_array();
  j.end_object();
  std::ofstream(options_.logs_dir / L"report.json", std::ios::binary) << j.str() << '\n';
}

}  // namespace am::rt
