#pragma once

// The title's memory: its allocations with their console attributes, and the console's physical
// pages (docs/PLAN.md, section 5.2).
//
// Physical pages are 64 KiB slices of one page-file-backed section the size of the console's
// title memory. The section is reserved, not committed: a page costs commit only once it is
// first touched. A mapped page that was never touched faults once; the fault handler commits it
// and the access runs again (the game maps 6 GiB of pages at start-up and fills them over time).
// Host calls that touch title memory in the kernel, where nothing faults, commit first
// (commit_range). The title maps pages into its own reservations, at several addresses
// if it likes, as on the console: each run of consecutive pages becomes one view of the section,
// placed over a placeholder (VirtualAlloc2 + MapViewOfFile3).
//
// Page numbers are opaque to the title. Ours count 4 KiB frames, so a 64 KiB page's number is a
// multiple of 16, and stay below 2^25: the game keeps them shifted right by 4 in 21 bits.

#include <windows.h>

#include <cstdint>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/memory/console_flags.h"

namespace am::rt::mem {

struct MemoryOptions {
  uint64_t physical_bytes = 0;  // size of the physical page pool
  uint64_t commit_cap = 0;      // 0: none; else the pool's commit plus the process's private
                                // commit stays below it (a job's limit counts only the latter)
  // Of it, the part committed up front, which the GPU can open in place (a section committed on
  // touch cannot be): pages allocated without flag 0x100 come from it while it has room.
  uint64_t gpu_pool_bytes = 1ull << 30;
};

struct MemoryStats {
  uint64_t physical_capacity = 0;   // bytes
  uint64_t physical_allocated = 0;  // bytes, now
  uint64_t physical_peak = 0;       // bytes, most at once
  uint64_t physical_committed = 0;  // bytes of the pool ever touched (commit stays with the section)
  uint64_t views = 0;               // mapped views now
  uint64_t map_calls = 0;           // XMemMapPhysicalPages(Scatter) calls
  uint64_t reservations = 0;        // physical-page reservations now
  uint64_t regions = 0;             // allocations with console attributes now
  uint64_t refused = 0;             // requests refused (also counted: host failures)
};

class TitleMemory {
 public:
  explicit TitleMemory(MemoryOptions options);
  ~TitleMemory();  // unmaps and releases everything it made (tests)
  TitleMemory(const TitleMemory&) = delete;
  TitleMemory& operator=(const TitleMemory&) = delete;

  // XMemVirtualAlloc and the console's VirtualAlloc. nullptr with the last error set on failure.
  void* allocate(const ConsoleRequest& request);
  // VirtualFree. MEM_RELEASE of a physical-page reservation releases all of it, views included;
  // MEM_RELEASE with the allocation's own size (the console allows it) releases it.
  bool free(void* address, uint64_t size, uint32_t type);
  // VirtualProtect with console bits.
  bool protect(void* address, uint64_t size, uint32_t new_protect, uint32_t* old_protect);
  // XMemVirtualQuery's flags for an address: an allocation's own, CPU memory for other committed
  // memory, 0 for free or reserved memory we do not know.
  uint64_t xmem_flags(const void* address) const;
  // Whether `address` is the start of an allocation made here (XMemFreeDefault asks).
  bool is_allocation(const void* address) const;

  // XMemAllocatePhysicalPages: all `count` pages or none.
  bool allocate_pages(uint64_t flags, uint64_t count, uint64_t* pages);
  bool free_pages(uint64_t count, const uint64_t* pages);
  // XMemMapPhysicalPages: maps `count` pages at `address` in place of what is mapped there;
  // `pages` null unmaps. Returns `address`, or nullptr when a page is unknown or the host fails.
  void* map_pages(void* address, uint64_t count, const uint64_t* pages);
  // The console GPU's page mapping copy (queue slot 31): maps at `to` the physical pages mapped
  // at `from`, `count` pages in a row; `repeat`: the one page at `from`, `count` times. False
  // when a source page has no physical page mapped.
  bool copy_mappings(uint64_t to, uint64_t from, uint64_t count, bool repeat);
  // XMemMapPhysicalPagesScatter: one page per address; `pages` null or a page 0 unmaps.
  bool map_pages_scatter(void* const* addresses, uint64_t count, const uint64_t* pages);
  // Writes bytes [offset, offset + size) of the pages listed (64 KiB each, in order), mapped
  // anywhere or not (DirectStorage destinations). False if a page is unknown.
  bool write_pages(const uint64_t* pages, uint64_t page_count, uint64_t offset, const uint8_t* data, uint64_t size);

  // The fault handler's part: commits the never-touched physical page under `address` for an
  // access of `kind` (0 read, 1 write, 8 execute). True when the access can run again.
  bool commit_on_fault(uint64_t address, uint64_t kind);
  // Commits the physical pages under [address, address + size) now. False (last error
  // ERROR_COMMITMENT_LIMIT) when that would pass the memory limit.
  bool commit_range(const void* address, uint64_t size);
  // Whether `address` is in a view of physical pages, and the protection its pages get.
  bool in_physical_view(const void* address, uint32_t* protection = nullptr) const;

  MemoryStats stats() const;

  // The GPU's view of physical pages: the committed pool's section (null until pages exist),
  // its size in pages, and every mapping change of views: `pool_page` is the first page's index
  // in the committed pool, or -1 when the pages are unmapped or in the lazy pool. The listener
  // runs under the title memory's lock and must not call back into it.
  using ViewListener = void (*)(uint64_t address, uint64_t pages, int64_t pool_page);
  HANDLE gpu_pool_section() const;
  uint64_t gpu_pool_pages() const;
  void set_view_listener(ViewListener listener);  // also called now for the views there are

 private:
  struct Region {
    uint64_t size = 0;
    uint64_t page_size = 0;
    uint64_t xmem_flags = 0;
    uint32_t cpu_protect = 0;  // what mapped physical pages get
    uint32_t gpu_access = 0;
    bool physical = false;     // a console physical-page reservation, kept as host placeholders
  };
  struct View {
    uint64_t size = 0;
    uint64_t first = 0;  // pool index of its first page
  };

  // ---- under mutex_ ----
  const Region* region_at(uint64_t address, uint64_t* base) const;
  void add_region(uint64_t base, const Region& region);  // replaces what overlaps it
  void cut_regions(uint64_t base, uint64_t size);
  bool ensure_pool();
  bool page_index(uint64_t number, uint64_t* index) const;
  bool map_view(uint64_t at, uint64_t pages, uint64_t first);
  const View* view_at(uint64_t address, uint64_t* at) const;
  bool commit_page(uint64_t at, const View& view, uint64_t page);
  uint32_t view_protection(uint64_t at) const;
  void unmap(uint64_t base, uint64_t size);
  bool release_physical(uint64_t base, uint64_t size);
  bool commit_allowed(uint64_t first, uint64_t pages);

  MemoryOptions options_;
  mutable std::mutex mutex_;
  std::map<uint64_t, Region> regions_;  // by base; never overlapping
  std::map<uint64_t, View> views_;      // by address; never overlapping

  HANDLE section_ = nullptr;      // the lazy pool: pages [0, lazy_pages_)
  HANDLE gpu_section_ = nullptr;  // the committed pool: pages [lazy_pages_, capacity_)
  uint64_t lazy_pages_ = 0;
  uint64_t hint_gpu_ = 0;
  ViewListener listener_ = nullptr;
  // The section holding pool page `index`, and its offset there.
  HANDLE section_of(uint64_t index, uint64_t* offset) const;
  uint64_t capacity_ = 0;  // pages
  std::vector<uint64_t> allocated_, committed_;  // bitmaps over the pool
  uint64_t allocated_count_ = 0, committed_count_ = 0, peak_ = 0, hint_ = 0;
  uint64_t map_calls_ = 0, refused_ = 0;
  // Every address a view ever covered lies in [views_low_, views_high_): a cheap test first.
  std::atomic<uint64_t> views_low_{UINT64_MAX}, views_high_{0};
};

// VirtualAlloc2, MapViewOfFile3 and UnmapViewOfFile2: Windows 10 1803 and later.
bool host_supports_placeholders();

// A plain host allocation in the title's address range ([4 GiB, 2^40)), outside the bookkeeping
// above: for title images the loader has to move.
void* allocate_title_range(uint64_t size, uint32_t type, uint32_t protect);

// The process's title memory, made on first use with the options given here before.
void configure_title_memory(const MemoryOptions& options);
TitleMemory& title_memory();

}  // namespace am::rt::mem
