// Title memory: the console's allocation flags, physical pages mapped at several addresses, and
// the default XMemAlloc allocator. The request shapes are the game's own (docs/PLAN.md, M2.1).

#include <windows.h>

#include <cstring>

#include "runtime/console.h"
#include "runtime/memory/console_flags.h"
#include "runtime/memory/default_alloc.h"
#include "runtime/memory/title_memory.h"
#include "test.h"

namespace {

using namespace am::rt;
using namespace am::rt::mem;

constexpr uint64_t kKiB = 1024, kMiB = 1024 * kKiB, kGiB = 1024 * kMiB;

ConsoleRequest request(uint64_t address, uint64_t size, uint32_t type, uint64_t flags, uint32_t protect) {
  ConsoleRequest r;
  r.address = address;
  r.size = size;
  r.type = type;
  r.xmem_flags = flags;
  r.protect = protect;
  return r;
}

uint64_t addr(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint8_t* at(void* p, uint64_t offset) { return static_cast<uint8_t*>(p) + offset; }

DWORD state(const void* p) {
  MEMORY_BASIC_INFORMATION mbi{};
  VirtualQuery(p, &mbi, sizeof mbi);
  return mbi.State;
}

}  // namespace

TEST_CASE(memory_translate_physical_reservation) {
  // The game's CPU and GPU ranges: 64 KiB pages, reserved, physical pages mapped later.
  const HostRequest cpu = translate(request(0, 24 * kGiB, 0x20402000, 0x21, PAGE_READWRITE));
  CHECK(cpu.action == Action::Host);
  CHECK(cpu.physical_reservation);
  CHECK(cpu.page_size == kPage64K);
  CHECK(cpu.type == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER));
  CHECK(cpu.protect == PAGE_NOACCESS);
  CHECK(cpu.cpu_protect == PAGE_READWRITE);
  CHECK(cpu.lowest == kTitleLowest && cpu.highest == kTitleHighest);
  const HostRequest gpu = translate(request(0, 24 * kGiB, 0x20402000, 3, 0x801));
  CHECK(gpu.cpu_protect == PAGE_NOACCESS);
  CHECK(gpu.gpu_access == 0x800);
}

TEST_CASE(memory_translate_large_pages_and_gpu_bits) {
  const HostRequest h = translate(request(0, 3 * kMiB, 0x20003000, 0x283, 0x22004));
  CHECK(h.action == Action::Host && !h.physical_reservation);
  CHECK(h.page_size == kPage2M && h.size == 4 * kMiB && h.alignment == kPage2M);
  CHECK(h.type == (MEM_RESERVE | MEM_COMMIT));
  CHECK(h.protect == PAGE_READWRITE);
  CHECK(h.gpu_access == 0x22000);
  // Write-combined and uncached CPU access is dropped: our layer reads that memory on the CPU.
  const HostRequest wc = translate(request(0, 64 * kKiB, 0x20401000, 0x283, 0x42404));
  CHECK(wc.type == (MEM_RESERVE | MEM_COMMIT) && wc.page_size == kPage64K);
  CHECK(wc.protect == PAGE_READWRITE && wc.dropped_protect == PAGE_WRITECOMBINE && wc.gpu_access == 0x42000);
  CHECK(host_protection(0x2000) == PAGE_NOACCESS);
  CHECK(host_protection(PAGE_READONLY | 0x1000) == PAGE_READONLY);
}

TEST_CASE(memory_translate_title_placeholders) {
  // The game's own placeholder, below 4 GiB, and a piece of it turned into a physical-page
  // reservation.
  const HostRequest p = translate(request(0, 8 * kMiB, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
  CHECK(p.action == Action::Host && !p.physical_reservation);
  CHECK(p.type == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER));
  CHECK(p.lowest == kLow32Lowest && p.highest == kLow32Highest);
  const HostRequest piece = translate(request(0x10000000, kMiB, 0x20406000, 0x27, 0x8404));
  CHECK(piece.action == Action::KeepPlaceholder && piece.physical_reservation);
  CHECK(piece.cpu_protect == PAGE_READWRITE && piece.gpu_access == 0x8000);
  CHECK(translate(request(0, kMiB, 0x20406000, 0x27, 0x8404)).action == Action::Refuse);
  CHECK(translate(request(0, 0, MEM_RESERVE, 0, PAGE_READWRITE)).action == Action::Refuse);
  CHECK(translate(request(0, kMiB, MEM_RESERVE, 0x1021, PAGE_READWRITE)).unknown_flags == 0x1000);
}

TEST_CASE(memory_physical_pages_alias_and_unmap) {
  TitleMemory memory({16 * kPage64K, 0});
  void* a = memory.allocate(request(0, kMiB, 0x20402000, 0x21, PAGE_READWRITE));
  void* b = memory.allocate(request(0, kMiB, 0x20402000, 0x23, 0x2004));
  CHECK(a && b && addr(a) >= kTitleLowest && addr(b) <= kTitleHighest);
  CHECK(state(a) == MEM_RESERVE);
  CHECK(memory.xmem_flags(a) == 0x21);

  uint64_t pages[4] = {};
  CHECK(memory.allocate_pages(0x380, 4, pages));
  for (uint64_t p : pages) CHECK(p && p % 16 == 0 && (p >> 4) < (uint64_t{1} << 21));
  CHECK(pages[0] != pages[1] && pages[1] != pages[2] && pages[2] != pages[3]);

  CHECK(memory.map_pages(a, 4, pages) == a);
  CHECK(state(a) == MEM_RESERVE);  // mapped, committed on first touch
  *at(a, 0) = 1;
  std::memset(at(a, 2 * kPage64K), 0x5A, kPage64K);
  CHECK(state(a) == MEM_COMMIT && state(at(a, 2 * kPage64K)) == MEM_COMMIT && state(at(a, kPage64K)) == MEM_RESERVE);
  // The same physical page at a second address shows the same bytes.
  CHECK(memory.map_pages(b, 1, pages + 2) == b);
  CHECK(*at(b, 0) == 0x5A && *at(b, kPage64K - 1) == 0x5A);
  *at(b, 100) = 0x11;
  CHECK(*at(a, 2 * kPage64K + 100) == 0x11);

  // Unmapping the second page leaves the first and third mapped.
  CHECK(memory.map_pages(at(a, kPage64K), 1, nullptr) == at(a, kPage64K));
  CHECK(state(at(a, kPage64K)) == MEM_RESERVE);
  CHECK(state(a) == MEM_COMMIT && *at(a, 2 * kPage64K + 100) == 0x11);
  // Scatter mapping puts the fourth page there; page 0 unmaps.
  void* where[2] = {at(a, kPage64K), at(a, 3 * kPage64K)};
  const uint64_t scatter[2] = {pages[3], 0};
  CHECK(memory.map_pages_scatter(where, 2, scatter));
  CHECK(*at(a, kPage64K) == 0);  // a page never touched reads as zero
  CHECK(state(at(a, kPage64K)) == MEM_COMMIT && state(at(a, 3 * kPage64K)) == MEM_RESERVE);

  uint64_t too_many[16] = {};
  CHECK(!memory.allocate_pages(0x380, 13, too_many));
  const uint64_t unknown = 0x7770;
  CHECK(memory.map_pages(b, 1, &unknown) == nullptr);
  CHECK(memory.stats().physical_allocated == 4 * kPage64K && memory.stats().views >= 3);

  // Releasing a reservation takes its views with it, whatever pieces the placeholder became.
  CHECK(memory.free(a, 0, MEM_RELEASE));
  CHECK(state(a) == MEM_FREE && state(at(a, kMiB - kPage64K)) == MEM_FREE);
  CHECK(memory.free(b, kMiB, MEM_RELEASE));  // the console accepts the reservation's size
  CHECK(state(b) == MEM_FREE);
  CHECK(memory.free_pages(4, pages));
  CHECK(memory.stats().physical_allocated == 0 && memory.stats().views == 0);
}

TEST_CASE(memory_title_placeholder_pieces) {
  TitleMemory memory({16 * kPage64K, 0});
  void* p = memory.allocate(request(0, kMiB, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
  CHECK(p && addr(p) < (uint64_t{1} << 32));
  // The title splits a piece off and makes it a physical-page reservation for GPU code.
  CHECK(memory.free(p, 256 * kKiB, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
  CHECK(memory.allocate(request(addr(p), 256 * kKiB, 0x20406000, 0x27, 0x8404)) == p);
  CHECK(memory.xmem_flags(p) == 0x27 && memory.xmem_flags(at(p, 256 * kKiB)) == 7);
  uint64_t pages[2] = {};
  CHECK(memory.allocate_pages(0x280, 2, pages));
  CHECK(memory.map_pages(p, 2, pages) == p);
  *at(p, kPage64K + 1) = 0x42;
  CHECK(*at(p, kPage64K + 1) == 0x42);
  CHECK(memory.free(p, 0, MEM_RELEASE));
  CHECK(state(p) == MEM_FREE && state(at(p, 256 * kKiB)) == MEM_RESERVE);
  CHECK(memory.free(at(p, 256 * kKiB), 0, MEM_RELEASE));
  CHECK(state(at(p, 256 * kKiB)) == MEM_FREE);
}

TEST_CASE(memory_large_pages_low_memory_and_release_size) {
  TitleMemory memory({0, 0});
  void* big = memory.allocate(request(0, 100 * kKiB, 0x20003000, 0x283, 0x2004));
  CHECK(big && addr(big) % kPage2M == 0 && addr(big) >= kTitleLowest && addr(big) < kTitleHighest);
  std::memset(big, 1, 2 * kMiB);  // whole 2 MiB pages
  CHECK(memory.free(big, 2 * kMiB, MEM_RELEASE));
  CHECK(state(big) == MEM_FREE);
  void* low = memory.allocate(request(0, 64 * kKiB, MEM_RESERVE | MEM_COMMIT, 7, PAGE_READWRITE));
  CHECK(low && addr(low) < (uint64_t{1} << 32));
  CHECK(memory.free(low, 0, MEM_RELEASE));
  // No pool configured: no physical pages.
  uint64_t page = 0;
  CHECK(!memory.allocate_pages(0, 1, &page));
}

// A touch the fault handler declines stays a fault.
bool touch_faults(volatile uint8_t* p) {
  __try {
    *p = 1;
    return false;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return true;
  }
}

TEST_CASE(memory_commit_cap_refuses_cleanly) {
  TitleMemory memory({4 * kPage64K, 1});  // a cap no commit fits under
  void* a = memory.allocate(request(0, kMiB, 0x20402000, 0x21, PAGE_READWRITE));
  uint64_t page = 0;
  CHECK(a && memory.allocate_pages(0, 1, &page));
  CHECK(memory.map_pages(a, 1, &page) == a);  // mapping costs nothing
  CHECK(!memory.commit_range(a, 16));
  CHECK(GetLastError() == ERROR_COMMITMENT_LIMIT);
  CHECK(touch_faults(at(a, 8)));
  CHECK(state(a) == MEM_RESERVE);
  CHECK(memory.free(a, 0, MEM_RELEASE));
}

TEST_CASE(memory_physical_pages_commit_on_first_touch) {
  TitleMemory memory({8 * kPage64K, 0});
  void* a = memory.allocate(request(0, kMiB, 0x20402000, 0x21, PAGE_READWRITE));
  void* b = memory.allocate(request(0, kMiB, 0x20402000, 0x21, PAGE_READWRITE));
  uint64_t pages[3] = {};
  CHECK(a && b && memory.allocate_pages(0, 3, pages));
  CHECK(memory.map_pages(a, 3, pages) == a);
  CHECK(memory.in_physical_view(at(a, 2 * kPage64K + 5)) && !memory.in_physical_view(at(a, 3 * kPage64K)));
  CHECK(memory.stats().physical_committed == 0);

  // A write commits its page only.
  *at(a, kPage64K + 7) = 0x33;
  CHECK(memory.stats().physical_committed == kPage64K);
  // A page touched through one view is committed in every view of it, at once.
  CHECK(memory.map_pages(b, 1, pages + 1) == b);
  CHECK(state(b) == MEM_COMMIT && *at(b, 7) == 0x33);

  // What the kernel will touch is committed up front: here pages 0 and 2, not 1 again.
  CHECK(memory.commit_range(at(a, kPage64K - 1), kPage64K + 2));
  CHECK(state(a) == MEM_COMMIT && state(at(a, 2 * kPage64K)) == MEM_COMMIT);
  CHECK(memory.stats().physical_committed == 3 * kPage64K);
  CHECK(memory.commit_range(at(a, 5 * kPage64K), 100));  // outside every view: nothing to do

  CHECK(memory.free(a, 0, MEM_RELEASE) && memory.free(b, 0, MEM_RELEASE));
  CHECK(memory.free_pages(3, pages));
}

TEST_CASE(memory_default_allocator) {
  TitleMemory memory({0, 0});
  DefaultAllocator allocator(memory);
  // Alignment field (bits 24-28): 0 and 4 give 16 bytes, 12 gives 4 KiB, 16 gives 64 KiB.
  const uint64_t fields[] = {0, 4, 12, 16};
  const uint64_t alignments[] = {16, 16, 4096, 65536};
  for (int i = 0; i < 4; ++i) {
    const uint64_t attributes = fields[i] << 24 | uint64_t{1} << 29;
    void* p = allocator.allocate(100, attributes);
    CHECK(p && addr(p) % alignments[i] == 0 && addr(p) < kTitleHighest);
    std::memset(p, 0xAB, 100);
    allocator.free(p, attributes);
  }
  void* first = allocator.allocate(40, 0);
  allocator.free(first, 0);
  CHECK(allocator.allocate(40, 0) == first);  // the block is reused
  void* large = allocator.allocate(300 * kKiB, 0);
  CHECK(large && memory.is_allocation(large));
  std::memset(large, 1, 300 * kKiB);
  allocator.free(large, 0);
  CHECK(state(large) == MEM_FREE);
  CHECK(xalloc_alignment(0x1Full << 24) == (uint64_t{1} << 31));
}

TEST_CASE(console_profile_choice) {
  Console c = Console::SeriesX;
  CHECK(choose_console("auto", 16 * kGiB, &c) && c == Console::SeriesS);
  CHECK(choose_console("auto", 24 * kGiB - 300 * kMiB, &c) && c == Console::SeriesX);
  CHECK(choose_console("series_s", 64 * kGiB, &c) && c == Console::SeriesS);
  CHECK(choose_console("series_x", 8 * kGiB, &c) && c == Console::SeriesX);
  CHECK(!choose_console("series_z", 8 * kGiB, &c));
  CHECK(profile_of(Console::SeriesX).title_memory == 13 * kGiB + 512 * kMiB);
}
