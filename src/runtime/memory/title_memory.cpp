#include "runtime/memory/title_memory.h"

#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "core/text.h"
#include "runtime/log.h"

namespace am::rt::mem {
namespace {

constexpr uint64_t kPage = kPage64K;
constexpr uint64_t kFramesPerPage = kPage / kPage4K;
// Page numbers shifted right by 4 must fit in 21 bits (the game stores them so).
constexpr uint64_t kMaxPages = (uint64_t{1} << 21) - 2;

bool test_bit(const std::vector<uint64_t>& bits, uint64_t i) { return (bits[i >> 6] >> (i & 63)) & 1; }
void set_bit(std::vector<uint64_t>& bits, uint64_t i) { bits[i >> 6] |= uint64_t{1} << (i & 63); }
void clear_bit(std::vector<uint64_t>& bits, uint64_t i) { bits[i >> 6] &= ~(uint64_t{1} << (i & 63)); }

void* ptr(uint64_t address) { return reinterpret_cast<void*>(address); }

// Placeholders (Windows 10 1803 and later) are in the umbrella import libraries only, which do
// not mix with kernel32.lib: found at run time instead.
struct PlaceholderApi {
  decltype(&::VirtualAlloc2) virtual_alloc2 = nullptr;
  decltype(&::MapViewOfFile3) map_view = nullptr;
  decltype(&::UnmapViewOfFile2) unmap_view = nullptr;
};

const PlaceholderApi& placeholders() {
  static const PlaceholderApi api = [] {
    PlaceholderApi a;
    if (HMODULE k = GetModuleHandleW(L"kernelbase.dll")) {
      a.virtual_alloc2 = reinterpret_cast<decltype(a.virtual_alloc2)>(GetProcAddress(k, "VirtualAlloc2"));
      a.map_view = reinterpret_cast<decltype(a.map_view)>(GetProcAddress(k, "MapViewOfFile3"));
      a.unmap_view = reinterpret_cast<decltype(a.unmap_view)>(GetProcAddress(k, "UnmapViewOfFile2"));
    }
    return a;
  }();
  return api;
}

uint64_t private_commit() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof counters;
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters);
  return counters.PrivateUsage;
}

// The size of the host allocation that starts at `base`; 0 when none starts there.
uint64_t allocation_extent(uint64_t base) {
  uint64_t p = base;
  MEMORY_BASIC_INFORMATION mbi;
  while (VirtualQuery(ptr(p), &mbi, sizeof mbi) == sizeof mbi && mbi.State != MEM_FREE &&
         reinterpret_cast<uint64_t>(mbi.AllocationBase) == base) {
    p = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return p - base;
}

void log_commit_failure(uint64_t bytes, DWORD error) {
  PERFORMANCE_INFORMATION perf{};
  perf.cb = sizeof perf;
  GetPerformanceInfo(&perf, sizeof perf);
  logf("memory: cannot commit %s of physical pages (error %lu); system commit %s of %s. Close other programs or "
       "enlarge the page file.",
       format_bytes(bytes).c_str(), error, format_bytes(uint64_t{perf.CommitTotal} * perf.PageSize).c_str(),
       format_bytes(uint64_t{perf.CommitLimit} * perf.PageSize).c_str());
}

// Every live TitleMemory, for the fault handler (tests make several).
std::mutex g_instances_mutex;
std::vector<TitleMemory*> g_instances;

LONG CALLBACK on_access_violation(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* e = info->ExceptionRecord;
  if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
  std::lock_guard<std::mutex> lock(g_instances_mutex);
  for (TitleMemory* m : g_instances) {
    if (m->commit_on_fault(e->ExceptionInformation[1], e->ExceptionInformation[0])) return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

bool access_allowed(uint32_t protection, uint64_t kind) {
  const uint32_t p = protection & 0xFF;
  if (protection & PAGE_GUARD) return false;
  if (kind == 1) return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
  if (kind == 8) return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
  return p != PAGE_NOACCESS && p != PAGE_EXECUTE;
}

}  // namespace

bool host_supports_placeholders() {
  const PlaceholderApi& a = placeholders();
  return a.virtual_alloc2 && a.map_view && a.unmap_view;
}

void* allocate_title_range(uint64_t size, uint32_t type, uint32_t protect) {
  if (!host_supports_placeholders()) return VirtualAlloc(nullptr, size, type, protect);
  MEM_ADDRESS_REQUIREMENTS requirements{};
  requirements.LowestStartingAddress = ptr(kTitleLowest);
  requirements.HighestEndingAddress = ptr(kTitleHighest);
  MEM_EXTENDED_PARAMETER parameter{};
  parameter.Type = MemExtendedParameterAddressRequirements;
  parameter.Pointer = &requirements;
  return placeholders().virtual_alloc2(GetCurrentProcess(), nullptr, size, type, protect, &parameter, 1);
}

TitleMemory::TitleMemory(MemoryOptions options) : options_(options) {
  static std::once_flag once;
  std::call_once(once, [] { AddVectoredExceptionHandler(1, &on_access_violation); });
  std::lock_guard<std::mutex> lock(g_instances_mutex);
  g_instances.push_back(this);
}

TitleMemory::~TitleMemory() {
  {
    std::lock_guard<std::mutex> lock(g_instances_mutex);
    std::erase(g_instances, this);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto regions = regions_;
  for (const auto& [base, r] : regions) {
    if (r.physical) release_physical(base, r.size);
    else VirtualFree(ptr(base), 0, MEM_RELEASE);
  }
  for (const auto& [at, v] : views_) placeholders().unmap_view(GetCurrentProcess(), ptr(at), 0);
  if (section_) CloseHandle(section_);
  if (gpu_section_) CloseHandle(gpu_section_);
}

// ---- allocations ----

const TitleMemory::Region* TitleMemory::region_at(uint64_t address, uint64_t* base) const {
  auto it = regions_.upper_bound(address);
  if (it == regions_.begin()) return nullptr;
  --it;
  if (address - it->first >= it->second.size) return nullptr;
  *base = it->first;
  return &it->second;
}

void TitleMemory::cut_regions(uint64_t base, uint64_t size) {
  const uint64_t end = base + size;
  auto it = regions_.upper_bound(base);
  if (it != regions_.begin() && std::prev(it)->first + std::prev(it)->second.size > base) --it;
  while (it != regions_.end() && it->first < end) {
    const uint64_t at = it->first;
    const Region r = it->second;
    it = regions_.erase(it);
    if (at < base) {
      Region before = r;
      before.size = base - at;
      regions_.emplace(at, before);
    }
    if (at + r.size > end) {
      Region after = r;
      after.size = at + r.size - end;
      it = regions_.emplace(end, after).first;
      ++it;
    }
  }
}

void TitleMemory::add_region(uint64_t base, const Region& region) {
  cut_regions(base, region.size);
  regions_[base] = region;
}

void* TitleMemory::allocate(const ConsoleRequest& request) {
  const HostRequest h = translate(request);
  std::lock_guard<std::mutex> lock(mutex_);
  if (h.action == Action::Refuse) {
    ++refused_;
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  Region region;
  region.size = h.size;
  region.page_size = h.page_size;
  region.xmem_flags = request.xmem_flags;
  region.cpu_protect = h.cpu_protect;
  region.gpu_access = h.gpu_access;
  region.physical = h.physical_reservation;

  if (h.action == Action::KeepPlaceholder) {
    // A piece the title split off a placeholder of its own: it still is one on the host.
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr(request.address), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_RESERVE ||
        reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize < request.address + h.size) {
      ++refused_;
      SetLastError(ERROR_INVALID_ADDRESS);
      return nullptr;
    }
    add_region(request.address, region);
    return ptr(request.address);
  }
  if (request.address && (h.type & MEM_COMMIT)) {
    uint64_t base = 0;
    if (const Region* r = region_at(request.address, &base); r && r->physical) {
      // The game never commits into a physical-page reservation; a placeholder cannot be.
      logf("memory: refused a commit at 0x%s inside a physical-page reservation", hex(request.address).c_str());
      ++refused_;
      SetLastError(ERROR_INVALID_ADDRESS);
      return nullptr;
    }
  }
  MEM_ADDRESS_REQUIREMENTS requirements{};
  MEM_EXTENDED_PARAMETER parameter{};
  ULONG parameters = 0;
  if (!request.address) {
    requirements.LowestStartingAddress = ptr(h.lowest);
    requirements.HighestEndingAddress = ptr(h.highest);
    requirements.Alignment = h.alignment;
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    parameters = 1;
  }
  void* p = placeholders().virtual_alloc2(GetCurrentProcess(), ptr(request.address), h.size, h.type, h.protect,
                          parameters ? &parameter : nullptr, parameters);
  if (!p) {
    ++refused_;
    return nullptr;
  }
  if (h.type & MEM_RESERVE) add_region(reinterpret_cast<uint64_t>(p), region);
  return p;
}

bool TitleMemory::free(void* address, uint64_t size, uint32_t type) {
  const uint64_t a = reinterpret_cast<uint64_t>(address);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!(type & MEM_RELEASE) || (type & (MEM_PRESERVE_PLACEHOLDER | MEM_COALESCE_PLACEHOLDERS))) {
    return VirtualFree(address, size, type) != 0;
  }
  if (auto it = regions_.find(a); it != regions_.end() && it->second.physical) {
    const uint64_t region_size = it->second.size;
    if (size && size != region_size) {
      SetLastError(ERROR_INVALID_PARAMETER);
      return false;
    }
    regions_.erase(it);
    return release_physical(a, region_size);
  }
  const uint64_t extent = allocation_extent(a);
  if (size && size == extent) size = 0;  // the console accepts the allocation's own size
  if (!VirtualFree(address, size, type)) return false;
  if (extent) cut_regions(a, extent);
  return true;
}

bool TitleMemory::release_physical(uint64_t base, uint64_t size) {
  unmap(base, size);
  const uint64_t end = base + size;
  bool ok = true;
  for (uint64_t p = base; p < end;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr(p), &mbi, sizeof mbi) != sizeof mbi) break;
    const uint64_t piece = reinterpret_cast<uint64_t>(mbi.BaseAddress);
    const uint64_t next = std::min(end, piece + mbi.RegionSize);
    if (mbi.State == MEM_RESERVE) {
      // Each split of the placeholder is a host allocation of its own; one reaching past the
      // reservation's edges is cut there first.
      if (reinterpret_cast<uint64_t>(mbi.AllocationBase) < p || piece + mbi.RegionSize > end) {
        VirtualFree(ptr(p), next - p, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
      }
      if (!VirtualFree(ptr(p), 0, MEM_RELEASE)) ok = false;
    } else if (mbi.State != MEM_FREE) {
      logf("memory: 0x%s in a physical-page reservation is neither a placeholder nor ours", hex(p).c_str());
      ok = false;
    }
    p = next;
  }
  return ok;
}

bool TitleMemory::protect(void* address, uint64_t size, uint32_t new_protect, uint32_t* old_protect) {
  DWORD old = 0;
  const BOOL ok = VirtualProtect(address, size, host_protection(new_protect), &old);
  if (old_protect) *old_protect = old;
  return ok != 0;
}

uint64_t TitleMemory::xmem_flags(const void* address) const {
  const uint64_t a = reinterpret_cast<uint64_t>(address);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t base = 0;
    if (const Region* r = region_at(a, &base)) return r->xmem_flags;
  }
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(address, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT) return kXMemCpu;
  return 0;
}

bool TitleMemory::is_allocation(const void* address) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return regions_.count(reinterpret_cast<uint64_t>(address)) != 0;
}

// ---- physical pages ----

bool TitleMemory::ensure_pool() {
  if (section_) return true;
  capacity_ = std::min(options_.physical_bytes / kPage, kMaxPages);
  if (!capacity_) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return false;
  }
  // The committed pool comes on top of the console's budget, which stays whole in the lazy pool:
  // the game asks for most of its budget in one allocation (6.1 GiB of a Series S's 7.6 GiB),
  // which must fit even when its small batches sit in the lazy pool too. At most half the
  // budget, and never more than the cap leaves room for.
  uint64_t gpu_pages = std::min(options_.gpu_pool_bytes / kPage, capacity_ / 2);
  if (options_.commit_cap && gpu_pages * kPage > options_.commit_cap / 4) gpu_pages = options_.commit_cap / 4 / kPage;
  if (capacity_ + gpu_pages > kMaxPages) gpu_pages = kMaxPages - capacity_;
  lazy_pages_ = capacity_;
  capacity_ += gpu_pages;
  const uint64_t bytes = lazy_pages_ * kPage;
  section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
  if (!section_) {
    logf("memory: cannot create the physical page pool of %s (error %lu)", format_bytes(bytes).c_str(), GetLastError());
    capacity_ = 0;
    return false;
  }
  if (gpu_pages) {
    const uint64_t gbytes = gpu_pages * kPage;
    gpu_section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                      static_cast<DWORD>(gbytes >> 32), static_cast<DWORD>(gbytes), nullptr);
    if (!gpu_section_) {
      logf("memory: no committed page pool of %s (error %lu); all pages commit on touch", format_bytes(gbytes).c_str(),
           GetLastError());
      capacity_ = lazy_pages_;
    }
  } else {
    capacity_ = lazy_pages_;
  }
  allocated_.assign((capacity_ + 63) / 64, 0);
  committed_.assign((capacity_ + 63) / 64, 0);
  for (uint64_t i = lazy_pages_; i < capacity_; ++i) set_bit(committed_, i);  // committed with the section
  committed_count_ += capacity_ - lazy_pages_;
  logf("memory: physical page pool of %s (%llu pages of 64 KiB), %s of them committed for the GPU",
       format_bytes(capacity_ * kPage).c_str(), static_cast<unsigned long long>(capacity_),
       format_bytes((capacity_ - lazy_pages_) * kPage).c_str());
  return true;
}

HANDLE TitleMemory::section_of(uint64_t index, uint64_t* offset) const {
  if (index < lazy_pages_) {
    *offset = index * kPage;
    return section_;
  }
  *offset = (index - lazy_pages_) * kPage;
  return gpu_section_;
}

HANDLE TitleMemory::gpu_pool_section() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capacity_ > lazy_pages_ ? gpu_section_ : nullptr;
}

uint64_t TitleMemory::gpu_pool_pages() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capacity_ - lazy_pages_;
}

void TitleMemory::set_view_listener(ViewListener listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = listener;
  if (!listener) return;
  for (const auto& [at, v] : views_) {
    listener(at, v.size / kPage, v.first >= lazy_pages_ ? static_cast<int64_t>(v.first - lazy_pages_) : -1);
  }
}

bool TitleMemory::page_index(uint64_t number, uint64_t* index) const {
  if (number % kFramesPerPage || number < kFramesPerPage) return false;
  const uint64_t i = number / kFramesPerPage - 1;
  if (i >= capacity_ || !test_bit(allocated_, i)) return false;
  *index = i;
  return true;
}

bool TitleMemory::allocate_pages(uint64_t flags, uint64_t count, uint64_t* pages) {
  if (!count || !pages) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ensure_pool()) return false;
  if (count > capacity_ - allocated_count_) {
    logf("memory: %llu physical pages asked for, %llu free", static_cast<unsigned long long>(count),
         static_cast<unsigned long long>(capacity_ - allocated_count_));
    ++refused_;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return false;
  }
  // Pages without flag 0x100 (the game's small batches: descriptors, per-frame data) come from
  // the committed pool while it has room; the rest (the streaming heap) from the lazy pool.
  uint64_t lo = 0, hi = lazy_pages_;
  uint64_t free_gpu = 0;
  if (!(flags & 0x100) && capacity_ > lazy_pages_) {
    for (uint64_t i = lazy_pages_; i < capacity_; ++i) free_gpu += !test_bit(allocated_, i);
    if (free_gpu >= count) lo = lazy_pages_, hi = capacity_;
  }
  uint64_t free_here = 0;
  if (hi == lazy_pages_) {
    for (uint64_t i = 0; i < lazy_pages_; i += 64) {
      for (uint64_t k = i; k < std::min(i + 64, lazy_pages_); ++k) free_here += !test_bit(allocated_, k);
    }
    if (free_here < count) {
      logf("memory: %llu physical pages asked for, %llu free in the pool for them", static_cast<unsigned long long>(count),
           static_cast<unsigned long long>(free_here));
      ++refused_;
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return false;
    }
  }
  uint64_t& hint = lo ? hint_gpu_ : hint_;
  uint64_t i = std::max(hint, lo), given = 0;
  while (given < count) {
    if (i >= hi) i = lo;
    if ((i & 63) == 0 && i + 64 <= hi && allocated_[i >> 6] == ~uint64_t{0}) {
      i += 64;
      continue;
    }
    if (!test_bit(allocated_, i)) {
      set_bit(allocated_, i);
      pages[given++] = (i + 1) * kFramesPerPage;
    }
    ++i;
  }
  hint = i;
  allocated_count_ += count;
  peak_ = std::max(peak_, allocated_count_);
  logf("memory: %s of physical pages (flags 0x%s), %s in use", format_bytes(count * kPage).c_str(), hex(flags).c_str(),
       format_bytes(allocated_count_ * kPage).c_str());
  return true;
}

bool TitleMemory::free_pages(uint64_t count, const uint64_t* pages) {
  if (!pages && count) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  bool ok = true;
  for (uint64_t k = 0; k < count; ++k) {
    uint64_t i = 0;
    if (!page_index(pages[k], &i)) {
      ok = false;
      continue;
    }
    // Views still showing the page keep showing it until unmapped, as on the console.
    clear_bit(allocated_, i);
    --allocated_count_;
  }
  if (!ok) SetLastError(ERROR_INVALID_PARAMETER);
  return ok;
}

bool TitleMemory::commit_allowed(uint64_t first, uint64_t pages) {
  if (!options_.commit_cap) return true;
  uint64_t fresh = 0;
  for (uint64_t i = first; i < first + pages; ++i) fresh += !test_bit(committed_, i);
  if (!fresh) return true;
  const uint64_t total = private_commit() + (committed_count_ + fresh) * kPage;
  if (total <= options_.commit_cap) return true;
  logf("memory: committing %s more would pass the memory limit of %s", format_bytes(fresh * kPage).c_str(),
       format_bytes(options_.commit_cap).c_str());
  SetLastError(ERROR_COMMITMENT_LIMIT);
  return false;
}

bool TitleMemory::map_view(uint64_t at, uint64_t pages, uint64_t first) {
  const uint64_t len = pages * kPage;
  // Make [at, at + len) a placeholder of its own; fails harmlessly when it already is one.
  VirtualFree(ptr(at), len, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  uint64_t offset = 0;
  HANDLE section = section_of(first, &offset);
  void* view = placeholders().map_view(section, GetCurrentProcess(), ptr(at), offset, len, MEM_REPLACE_PLACEHOLDER,
                              PAGE_READWRITE, nullptr, 0);
  if (view != ptr(at)) {
    logf("memory: cannot map %s of physical pages at 0x%s (error %lu): not a placeholder?", format_bytes(len).c_str(),
         hex(at).c_str(), GetLastError());
    if (view) placeholders().unmap_view(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER);
    return false;
  }
  views_[at] = View{len, first};
  if (listener_) listener_(at, pages, first >= lazy_pages_ ? static_cast<int64_t>(first - lazy_pages_) : -1);
  uint64_t low = views_low_.load(), high = views_high_.load();
  while (at < low && !views_low_.compare_exchange_weak(low, at)) {
  }
  while (at + len > high && !views_high_.compare_exchange_weak(high, at + len)) {
  }
  // Pages touched before show at once (commit belongs to the section) and get the reservation's
  // protection here; the others get it when first touched.
  const uint32_t protection = view_protection(at);
  if (protection != PAGE_READWRITE) {
    for (uint64_t k = 0; k < pages;) {
      if (!test_bit(committed_, first + k)) {
        ++k;
        continue;
      }
      uint64_t run = 1;
      while (k + run < pages && test_bit(committed_, first + k + run)) ++run;
      DWORD old = 0;
      if (!VirtualProtect(ptr(at + k * kPage), run * kPage, protection, &old)) {
        logf("memory: protection 0x%s for physical pages at 0x%s refused (error %lu)", hex(protection).c_str(),
             hex(at + k * kPage).c_str(), GetLastError());
      }
      k += run;
    }
  }
  return true;
}

uint32_t TitleMemory::view_protection(uint64_t at) const {
  uint64_t base = 0;
  const Region* r = region_at(at, &base);
  return r && r->cpu_protect ? r->cpu_protect : PAGE_READWRITE;
}

const TitleMemory::View* TitleMemory::view_at(uint64_t address, uint64_t* at) const {
  auto it = views_.upper_bound(address);
  if (it == views_.begin()) return nullptr;
  --it;
  if (address - it->first >= it->second.size) return nullptr;
  *at = it->first;
  return &it->second;
}

bool TitleMemory::commit_page(uint64_t at, const View& view, uint64_t page) {
  const uint64_t index = view.first + page;
  if (test_bit(committed_, index)) return true;
  if (!commit_allowed(index, 1)) return false;
  void* p = ptr(at + page * kPage);
  if (!VirtualAlloc(p, kPage, MEM_COMMIT, PAGE_READWRITE)) {
    const DWORD error = GetLastError();
    log_commit_failure(kPage, error);
    SetLastError(error);
    return false;
  }
  set_bit(committed_, index);
  ++committed_count_;
  const uint32_t protection = view_protection(at);
  if (protection != PAGE_READWRITE) {
    DWORD old = 0;
    VirtualProtect(p, kPage, protection, &old);
  }
  return true;
}

bool TitleMemory::commit_on_fault(uint64_t address, uint64_t kind) {
  if (address < views_low_.load(std::memory_order_relaxed) || address >= views_high_.load(std::memory_order_relaxed)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t at = 0;
  const View* v = view_at(address, &at);
  if (!v) return false;
  const uint64_t page = (address - at) / kPage;
  if (test_bit(committed_, v->first + page)) {
    // Committed already: another thread got here first (the access can run again), or the
    // page's protection refuses this access (a real fault).
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(ptr(address), &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT &&
           access_allowed(mbi.Protect, kind);
  }
  return commit_page(at, *v, page);
}

bool TitleMemory::commit_range(const void* address, uint64_t size) {
  const uint64_t a = reinterpret_cast<uint64_t>(address);
  if (!size || a + size <= views_low_.load(std::memory_order_relaxed) ||
      a >= views_high_.load(std::memory_order_relaxed)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t end = a + size;
  auto it = views_.upper_bound(a);
  if (it != views_.begin() && std::prev(it)->first + std::prev(it)->second.size > a) --it;
  for (; it != views_.end() && it->first < end; ++it) {
    const uint64_t at = it->first;
    const uint64_t from = (std::max(a, at) - at) / kPage;
    const uint64_t to = (std::min(end, at + it->second.size) - at + kPage - 1) / kPage;
    for (uint64_t page = from; page < to; ++page) {
      if (!commit_page(at, it->second, page)) return false;
    }
  }
  return true;
}

bool TitleMemory::in_physical_view(const void* address, uint32_t* protection) const {
  const uint64_t a = reinterpret_cast<uint64_t>(address);
  if (a < views_low_.load(std::memory_order_relaxed) || a >= views_high_.load(std::memory_order_relaxed)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t at = 0;
  if (!view_at(a, &at)) return false;
  if (protection) *protection = view_protection(at);
  return true;
}

void TitleMemory::unmap(uint64_t base, uint64_t size) {
  if (!size) return;
  const uint64_t end = base + size;
  auto it = views_.upper_bound(base);
  if (it != views_.begin() && std::prev(it)->first + std::prev(it)->second.size > base) --it;
  std::vector<std::pair<uint64_t, View>> hit;
  for (; it != views_.end() && it->first < end; ++it) hit.push_back(*it);
  for (const auto& [at, v] : hit) {
    views_.erase(at);
    if (listener_) listener_(at, v.size / kPage, -1);
    placeholders().unmap_view(GetCurrentProcess(), ptr(at), MEM_PRESERVE_PLACEHOLDER);
    // The parts of the view outside the range stay mapped.
    if (at < base) map_view(at, (base - at) / kPage, v.first);
    if (at + v.size > end) map_view(end, (at + v.size - end) / kPage, v.first + (end - at) / kPage);
  }
}

void* TitleMemory::map_pages(void* address, uint64_t count, const uint64_t* pages) {
  const uint64_t a = reinterpret_cast<uint64_t>(address);
  if (a % kPage || count > (uint64_t{1} << 40) / kPage) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  if (!count) return address;
  std::lock_guard<std::mutex> lock(mutex_);
  ++map_calls_;
  if (!pages) {
    unmap(a, count * kPage);
    return address;
  }
  std::vector<uint64_t> index(count);
  for (uint64_t k = 0; k < count; ++k) {
    if (!page_index(pages[k], &index[k])) {
      logf("memory: XMemMapPhysicalPages at 0x%s: page 0x%s is not allocated", hex(a).c_str(), hex(pages[k]).c_str());
      ++refused_;
      SetLastError(ERROR_INVALID_PARAMETER);
      return nullptr;
    }
  }
  unmap(a, count * kPage);
  for (uint64_t k = 0; k < count;) {
    uint64_t run = 1;
    while (k + run < count && index[k + run] == index[k] + run && (index[k] < lazy_pages_) == (index[k + run] < lazy_pages_)) ++run;
    if (!map_view(a + k * kPage, run, index[k])) {
      unmap(a, k * kPage);
      ++refused_;
      return nullptr;
    }
    k += run;
  }
  return address;
}

bool TitleMemory::map_pages_scatter(void* const* addresses, uint64_t count, const uint64_t* pages) {
  if (!addresses && count) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  for (uint64_t k = 0; k < count; ++k) {
    const uint64_t page = pages ? pages[k] : 0;
    if (!map_pages(addresses[k], 1, page ? &page : nullptr)) return false;
  }
  return true;
}

bool TitleMemory::write_pages(const uint64_t* pages, uint64_t page_count, uint64_t offset, const uint8_t* data,
                              uint64_t size) {
  if (!size) return true;
  if (!pages || offset > page_count * kPage || size > page_count * kPage - offset) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  const uint64_t first = offset / kPage, last = (offset + size - 1) / kPage;
  std::vector<uint64_t> index(last - first + 1);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint64_t k = first; k <= last; ++k) {
      if (!page_index(pages[k], &index[k - first])) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
      }
    }
    uint64_t fresh = 0;
    for (uint64_t i : index) fresh += !test_bit(committed_, i);
    if (fresh && options_.commit_cap && private_commit() + (committed_count_ + fresh) * kPage > options_.commit_cap) {
      logf("memory: writing physical pages would pass the memory limit of %s", format_bytes(options_.commit_cap).c_str());
      SetLastError(ERROR_COMMITMENT_LIMIT);
      return false;
    }
    for (uint64_t i : index) {
      if (!test_bit(committed_, i)) {
        set_bit(committed_, i);
        ++committed_count_;
      }
    }
  }
  // One view of our own per run of consecutive pages, outside the lock: the copies are large.
  uint64_t done = 0, within = offset % kPage;
  for (size_t k = 0; k < index.size();) {
    size_t run = 1;
    while (k + run < index.size() && index[k + run] == index[k] + run && (index[k] < lazy_pages_) == (index[k + run] < lazy_pages_)) {
      ++run;
    }
    const uint64_t len = run * kPage;
    uint64_t at = 0;
    HANDLE section = section_of(index[k], &at);
    void* view = MapViewOfFile(section, FILE_MAP_WRITE, static_cast<DWORD>(at >> 32), static_cast<DWORD>(at), len);
    if (!view || !VirtualAlloc(view, len, MEM_COMMIT, PAGE_READWRITE)) {
      const DWORD error = GetLastError();
      if (view) UnmapViewOfFile(view);
      log_commit_failure(len, error);
      SetLastError(error);
      return false;
    }
    const uint64_t n = std::min(len - within, size - done);
    std::memcpy(static_cast<uint8_t*>(view) + within, data + done, n);
    UnmapViewOfFile(view);
    done += n;
    within = 0;
    k += run;
  }
  return true;
}

MemoryStats TitleMemory::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  MemoryStats s;
  s.physical_capacity = (capacity_ ? capacity_ : std::min(options_.physical_bytes / kPage, kMaxPages)) * kPage;
  s.physical_allocated = allocated_count_ * kPage;
  s.physical_peak = peak_ * kPage;
  s.physical_committed = committed_count_ * kPage;
  s.views = views_.size();
  s.map_calls = map_calls_;
  s.refused = refused_;
  for (const auto& [base, r] : regions_) {
    ++s.regions;
    s.reservations += r.physical;
  }
  return s;
}

// ---- the process's ----

namespace {
MemoryOptions g_options;
}

void configure_title_memory(const MemoryOptions& options) { g_options = options; }

TitleMemory& title_memory() {
  // Never destroyed: title threads may still run while the process ends.
  static TitleMemory* const memory = new TitleMemory(g_options);
  return *memory;
}

}  // namespace am::rt::mem
