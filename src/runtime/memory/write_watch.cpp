#include "runtime/memory/write_watch.h"

#include <windows.h>

#include <map>
#include <mutex>

namespace am::rt::mem {
namespace {

constexpr uint64_t kPage = 4096;

struct Watches {
  std::mutex lock;
  std::map<uint64_t, uint64_t> ranges;  // first -> end; they do not overlap
  std::vector<std::pair<uint64_t, uint64_t>> written;
};

Watches& watches() {
  static Watches* const w = new Watches;
  return *w;
}

// Under the lock: the watched range holding `address`, or end().
std::map<uint64_t, uint64_t>::iterator range_at(Watches& w, uint64_t address) {
  auto it = w.ranges.upper_bound(address);
  if (it == w.ranges.begin()) return w.ranges.end();
  --it;
  return address < it->second ? it : w.ranges.end();
}

// Under the lock: makes the range writable again and reports it.
void release(Watches& w, std::map<uint64_t, uint64_t>::iterator it) {
  DWORD old = 0;
  VirtualProtect(reinterpret_cast<void*>(it->first), it->second - it->first, PAGE_READWRITE, &old);
  w.written.emplace_back(it->first, it->second);
  w.ranges.erase(it);
}

// Whether [first, end) is all committed private memory that is read-write, or read-only from a
// watch of ours. Not views of physical pages: the title may write them through another view.
bool watchable(Watches& w, uint64_t first, uint64_t end) {
  for (uint64_t at = first; at < end;) {
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery(reinterpret_cast<void*>(at), &m, sizeof m) != sizeof m || m.State != MEM_COMMIT || m.Type != MEM_PRIVATE) {
      return false;
    }
    const bool ours = m.Protect == PAGE_READONLY && range_at(w, at) != w.ranges.end();
    if (m.Protect != PAGE_READWRITE && !ours) return false;
    at = reinterpret_cast<uint64_t>(m.BaseAddress) + m.RegionSize;
  }
  return true;
}

// The title's first write to a watched range: let through.
LONG CALLBACK on_write(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* e = info->ExceptionRecord;
  if (e->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->NumberParameters < 2 || e->ExceptionInformation[0] != 1) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  Watches& w = watches();
  std::lock_guard g(w.lock);
  auto it = range_at(w, e->ExceptionInformation[1]);
  if (it == w.ranges.end()) return EXCEPTION_CONTINUE_SEARCH;
  release(w, it);
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool watch_writes(uint64_t address, uint64_t size) {
  if (!size) return false;
  static std::once_flag once;
  std::call_once(once, [] { AddVectoredExceptionHandler(1, &on_write); });
  uint64_t first = address / kPage * kPage, end = (address + size + kPage - 1) / kPage * kPage;
  Watches& w = watches();
  std::lock_guard g(w.lock);
  if (!watchable(w, first, end)) return false;
  // Merged with the watches it overlaps.
  for (auto it = range_at(w, first) != w.ranges.end() ? range_at(w, first) : w.ranges.lower_bound(first);
       it != w.ranges.end() && it->first < end;) {
    first = std::min(first, it->first);
    end = std::max(end, it->second);
    it = w.ranges.erase(it);
  }
  DWORD old = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(first), end - first, PAGE_READONLY, &old)) {
    // Nothing is watched; pages a merged watch had protected are writable again (as written).
    VirtualProtect(reinterpret_cast<void*>(first), end - first, PAGE_READWRITE, &old);
    w.written.emplace_back(first, end);
    return false;
  }
  w.ranges[first] = end;
  return true;
}

void take_written(std::vector<std::pair<uint64_t, uint64_t>>& out) {
  Watches& w = watches();
  std::lock_guard g(w.lock);
  out.insert(out.end(), w.written.begin(), w.written.end());
  w.written.clear();
}

void release_for_host_write(const void* address, uint64_t size) {
  if (!size) return;
  const uint64_t first = reinterpret_cast<uint64_t>(address), end = first + size;
  Watches& w = watches();
  std::lock_guard g(w.lock);
  if (w.ranges.empty()) return;
  auto it = range_at(w, first);
  if (it == w.ranges.end()) it = w.ranges.lower_bound(first);
  while (it != w.ranges.end() && it->first < end) {
    auto next = std::next(it);
    release(w, it);
    it = next;
  }
}

}  // namespace am::rt::mem
