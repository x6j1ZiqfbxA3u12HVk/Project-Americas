#pragma once

#include <cstdint>

namespace am::host {

struct OsVersion {
  uint32_t major = 0, minor = 0, build = 0, revision = 0;
};
OsVersion os_version();

struct Topology {
  uint32_t packages = 0;
  uint32_t cores = 0;
  uint32_t logical = 0;
  uint32_t l3_caches = 0;
  uint64_t l3_bytes = 0;  // all L3 caches together
};
Topology processor_topology();

struct MemoryInfo {
  uint64_t total_physical = 0;
  uint64_t available_physical = 0;
  uint64_t commit_limit = 0;  // physical memory + page files
  uint64_t commit_total = 0;
  uint64_t page_size = 0;
  uint64_t large_page_minimum = 0;  // 0 when large pages are unsupported
  bool lock_pages_privilege = false;  // "Lock pages in memory": needed for large pages
};
MemoryInfo memory_info();

}  // namespace am::host
