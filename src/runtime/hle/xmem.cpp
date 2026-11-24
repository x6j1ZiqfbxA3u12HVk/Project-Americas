// xmem.dll and the console's VirtualAlloc family (plan, section 5.2): title memory with console
// attributes, physical pages, and XMemAlloc through the title's own allocator hooks.

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <variant>

#include "core/text.h"
#include "runtime/console.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"
#include "runtime/memory/default_alloc.h"
#include "runtime/memory/title_memory.h"
#include "runtime/process.h"

namespace am::rt::hle {
namespace {

using mem::title_memory;

bool is_self(HANDLE process) { return process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId(); }

// ---- virtual memory ----

void* WINAPI my_XMemVirtualAlloc(void* address, SIZE_T size, DWORD type, ULONG64 flags, DWORD protect) {
  mem::ConsoleRequest r;
  r.address = reinterpret_cast<uint64_t>(address);
  r.size = size;
  r.type = type;
  r.xmem_flags = flags;
  r.protect = protect;
  void* p = title_memory().allocate(r);
  // New allocations are rare (the game's code has 23 call sites): each is worth a line.
  static std::atomic<int> logged{0};
  if ((!address || !p) && logged.fetch_add(1, std::memory_order_relaxed) < 200) {
    logf("XMemVirtualAlloc(0x%s, %s, type 0x%s, flags 0x%s, protect 0x%s) = 0x%s%s", hex(r.address).c_str(),
         format_bytes(size).c_str(), hex(type).c_str(), hex(flags).c_str(), hex(protect).c_str(),
         hex(reinterpret_cast<uint64_t>(p)).c_str(), p ? "" : (" error " + std::to_string(GetLastError())).c_str());
  }
  return p;
}

// What XMemVirtualQuery fills: the host's MEMORY_BASIC_INFORMATION, then the allocation's XMem
// flags. The ray-tracing BVH builder checks State, Protect and the flags' low two bits.
struct XMemBasicInformation {
  MEMORY_BASIC_INFORMATION host;
  ULONG64 flags;
  ULONG64 tag;  // ours: always 0
};
static_assert(offsetof(XMemBasicInformation, flags) == 0x30 && sizeof(XMemBasicInformation) == 0x40);

BOOL WINAPI my_XMemVirtualQuery(HANDLE process, const void* address, XMemBasicInformation* out) {
  if (!out) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (!VirtualQueryEx(process, address, &out->host, sizeof out->host)) return FALSE;
  out->flags = out->host.State == MEM_FREE ? 0 : title_memory().xmem_flags(address);
  out->tag = 0;
  return TRUE;
}

LPVOID WINAPI my_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
  mem::ConsoleRequest r;
  r.address = reinterpret_cast<uint64_t>(address);
  r.size = size;
  r.type = type;
  r.xmem_flags = mem::kXMemCpu;
  r.protect = protect;
  return title_memory().allocate(r);
}
LPVOID WINAPI my_VirtualAllocEx(HANDLE process, LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
  if (!is_self(process)) {
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr;
  }
  return my_VirtualAlloc(address, size, type, protect);
}
BOOL WINAPI my_VirtualFree(LPVOID address, SIZE_T size, DWORD type) { return title_memory().free(address, size, type); }
BOOL WINAPI my_VirtualFreeEx(HANDLE process, LPVOID address, SIZE_T size, DWORD type) {
  if (!is_self(process)) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return my_VirtualFree(address, size, type);
}
BOOL WINAPI my_VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
  uint32_t previous = 0;
  const bool ok = title_memory().protect(address, size, protect, &previous);
  if (old) *old = previous;
  return ok;
}
BOOL WINAPI my_VirtualProtectEx(HANDLE process, LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
  if (!is_self(process)) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  return my_VirtualProtect(address, size, protect, old);
}
// The host's answers. Physical pages mapped into a reservation show as mapped views.
SIZE_T WINAPI my_VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION out, SIZE_T size) {
  return VirtualQuery(address, out, size);
}
SIZE_T WINAPI my_VirtualQueryEx(HANDLE process, LPCVOID address, PMEMORY_BASIC_INFORMATION out, SIZE_T size) {
  return VirtualQueryEx(process, address, out, size);
}

// ---- XMemAlloc ----

// The executable may export `__XMemAllocationHooks__`: its allocate and free routines, which
// XMemAlloc and XMemFree call for every module of the title. The game's hooks dispatch on the
// attributes' allocator id and fall back to XMemAllocDefault.
using AllocHook = void*(WINAPI*)(SIZE_T, ULONG64);
using FreeHook = void(WINAPI*)(void*, ULONG64);

void* const* hook_table() {
  static void* const* const table = [] {
    void* const* t = nullptr;
    if (TitleProcess* p = process(); p && p->executable()) {
      const MappedImage::Export e = p->executable()->find_export("__XMemAllocationHooks__");
      if (void* const* at = std::get_if<void*>(&e)) t = static_cast<void* const*>(*at);
    }
    logf("xmem: XMemAlloc %s", t ? "goes to the title's allocation hooks" : "uses the default allocator");
    return t;
  }();
  return table;
}

void* WINAPI my_XMemAllocDefault(SIZE_T size, ULONG64 attributes) {
  return mem::default_allocator().allocate(size, attributes);
}
void WINAPI my_XMemFreeDefault(void* address, ULONG64 attributes) { mem::default_allocator().free(address, attributes); }

void* WINAPI my_XMemAlloc(SIZE_T size, ULONG64 attributes) {
  if (void* const* t = hook_table(); t && t[0] && t[1]) return reinterpret_cast<AllocHook>(t[0])(size, attributes);
  return my_XMemAllocDefault(size, attributes);
}
void WINAPI my_XMemFree(void* address, ULONG64 attributes) {
  if (void* const* t = hook_table(); t && t[0] && t[1]) return reinterpret_cast<FreeHook>(t[1])(address, attributes);
  my_XMemFreeDefault(address, attributes);
}

// ---- physical pages ----

// `count` is in and out: pages wanted, pages given (all or none here).
BOOL WINAPI my_XMemAllocatePhysicalPages(ULONG64 flags, ULONG64* count, ULONG64* pages) {
  if (!count) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (!title_memory().allocate_pages(flags, *count, pages)) {
    *count = 0;
    return FALSE;
  }
  return TRUE;
}
BOOL WINAPI my_XMemFreePhysicalPages(ULONG64 count, const ULONG64* pages) { return title_memory().free_pages(count, pages); }
void* WINAPI my_XMemMapPhysicalPages(void* address, ULONG64 count, const ULONG64* pages) {
  return title_memory().map_pages(address, count, pages);
}
BOOL WINAPI my_XMemMapPhysicalPagesScatter(void* const* addresses, ULONG64 count, const ULONG64* pages) {
  return title_memory().map_pages_scatter(addresses, count, pages);
}

// ---- budget ----

// The leading fields every version shares; callers pass larger, zeroed structures (the game
// 0x78 bytes) and read title_limit, title_used, and in a tools-memory path the tools fields.
struct WorkingSetStatistics {
  ULONG64 title_limit;
  ULONG64 title_used;
  ULONG64 tools_limit;
  ULONG64 tools_used;
  ULONG stack_used;
  ULONG physical_pages;
  ULONG64 gpu_optimal_limit;
  ULONG64 gpu_optimal_used;
  ULONG64 tools_gpu_optimal_limit;
  ULONG64 tools_gpu_optimal_used;
  ULONG kernel_pool_used;
  ULONG kernel_pool_limit;
};
static_assert(sizeof(WorkingSetStatistics) == 0x50);

// The console's title memory as the budget; our process's private commit plus the physical
// pages the title holds as what it uses. No tools memory: a retail console.
BOOL WINAPI my_XMemGetWorkingSetStatistics(DWORD /*which*/, WorkingSetStatistics* out) {
  if (!out) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof counters;
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters);
  const ConsoleProfile& console = current_console();
  WorkingSetStatistics s{};
  s.title_limit = console.title_memory;
  s.title_used = counters.PrivateUsage + title_memory().stats().physical_allocated;
  s.gpu_optimal_limit = console.gpu_optimal;
  *out = s;
  return TRUE;
}

BOOL WINAPI my_XMemIsToolsMemAvailable() { return FALSE; }

// Moves memory between the title and tools pools. A retail console has no tools memory; the
// game calls this only on a path that asserts tools memory first.
BOOL WINAPI my_XMemTransferMemory(DWORD kind, SIZE_T size) {
  logf("XMemTransferMemory(%lu, %s) refused: no tools memory", kind, format_bytes(size).c_str());
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

#define AM_XMEM(name) Function{"xmem.dll", #name, reinterpret_cast<void*>(&my_##name)}
#define AM_WRAP(name) Function{nullptr, #name, reinterpret_cast<void*>(&my_##name)}

}  // namespace

void add_memory(std::vector<Function>& out) {
  out.insert(out.end(), {
      AM_XMEM(XMemVirtualAlloc), AM_XMEM(XMemVirtualQuery), AM_XMEM(XMemAlloc), AM_XMEM(XMemFree),
      AM_XMEM(XMemAllocDefault), AM_XMEM(XMemFreeDefault), AM_XMEM(XMemAllocatePhysicalPages),
      AM_XMEM(XMemFreePhysicalPages), AM_XMEM(XMemMapPhysicalPages), AM_XMEM(XMemMapPhysicalPagesScatter),
      AM_XMEM(XMemGetWorkingSetStatistics), AM_XMEM(XMemIsToolsMemAvailable), AM_XMEM(XMemTransferMemory),
      AM_WRAP(VirtualAlloc), AM_WRAP(VirtualAllocEx), AM_WRAP(VirtualFree), AM_WRAP(VirtualFreeEx),
      AM_WRAP(VirtualProtect), AM_WRAP(VirtualProtectEx), AM_WRAP(VirtualQuery), AM_WRAP(VirtualQueryEx),
  });
}

}  // namespace am::rt::hle
