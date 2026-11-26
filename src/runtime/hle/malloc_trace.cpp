// AMERICAS_TRACE_MALLOC (diagnosis): the title's CRT heap calls (malloc, calloc, realloc, free)
// go through here, and every 10 s the call stacks holding the most live bytes are logged,
// as module+offset. For finding what fills the process's commit.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::hle {
namespace {

constexpr int kFrames = 6;

struct Site {
  void* frames[kFrames] = {};
  uint64_t live = 0, count = 0;
};
struct Block {
  size_t size;
  uint64_t site;
};

struct Tracker {
  std::mutex lock;
  std::unordered_map<void*, Block> blocks;
  std::unordered_map<uint64_t, Site> sites;
};
Tracker& tracker() {
  static Tracker* const t = new Tracker;
  return *t;
}

std::string where(void* p) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<const char*>(p), &m) || !m) {
    char b[32];
    std::snprintf(b, sizeof b, "%p", p);
    return b;
  }
  char name[MAX_PATH] = {};
  GetModuleFileNameA(m, name, MAX_PATH);
  const char* base = std::strrchr(name, '\\');
  char b[MAX_PATH + 32];
  std::snprintf(b, sizeof b, "%s+0x%llx", base ? base + 1 : name,
                static_cast<unsigned long long>(static_cast<const char*>(p) - reinterpret_cast<const char*>(m)));
  return b;
}

void note(void* p, size_t size) {
  if (!p) return;
  void* frames[kFrames + 1] = {};
  const USHORT n = RtlCaptureStackBackTrace(2, kFrames, frames, nullptr);
  uint64_t key = 1469598103934665603ull;
  for (USHORT i = 0; i < n; ++i) key = (key ^ reinterpret_cast<uint64_t>(frames[i])) * 1099511628211ull;
  Tracker& t = tracker();
  std::lock_guard g(t.lock);
  Site& s = t.sites[key];
  if (!s.count) std::copy(frames, frames + kFrames, s.frames);
  s.live += size;
  ++s.count;
  t.blocks[p] = {size, key};
}
void forget(void* p) {
  if (!p) return;
  Tracker& t = tracker();
  std::lock_guard g(t.lock);
  auto it = t.blocks.find(p);
  if (it == t.blocks.end()) return;
  Site& s = t.sites[it->second.site];
  s.live -= it->second.size;
  --s.count;
  t.blocks.erase(it);
}

void* __cdecl my_malloc(size_t n) {
  void* p = std::malloc(n);
  note(p, n);
  return p;
}
void* __cdecl my_calloc(size_t n, size_t each) {
  void* p = std::calloc(n, each);
  note(p, n * each);
  return p;
}
void* __cdecl my_realloc(void* old, size_t n) {
  forget(old);
  void* p = std::realloc(old, n);
  note(p, n);
  return p;
}
void __cdecl my_free(void* p) {
  forget(p);
  std::free(p);
}

void reporter() {
  for (;;) {
    Sleep(10000);
    std::vector<Site> top;
    uint64_t total = 0, blocks = 0;
    {
      Tracker& t = tracker();
      std::lock_guard g(t.lock);
      blocks = t.blocks.size();
      for (const auto& [k, s] : t.sites) {
        total += s.live;
        top.push_back(s);
      }
    }
    std::sort(top.begin(), top.end(), [](const Site& a, const Site& b) { return a.live > b.live; });
    logf("malloc trace: %llu MiB live in %llu blocks", static_cast<unsigned long long>(total >> 20),
         static_cast<unsigned long long>(blocks));
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
      std::string stack;
      for (void* f : top[i].frames) {
        if (f) stack += " " + where(f);
      }
      logf("malloc trace:   %llu MiB in %llu blocks:%s", static_cast<unsigned long long>(top[i].live >> 20),
           static_cast<unsigned long long>(top[i].count), stack.c_str());
    }
  }
}

}  // namespace

void add_malloc_trace(std::vector<Function>& out) {
  if (!GetEnvironmentVariableA("AMERICAS_TRACE_MALLOC", nullptr, 0)) return;
  std::thread(reporter).detach();
  for (const char* dll : {"api-ms-win-crt-heap-l1-1-0.dll", "ucrtbase.dll"}) {
    out.push_back(Function{dll, "malloc", reinterpret_cast<void*>(&my_malloc)});
    out.push_back(Function{dll, "calloc", reinterpret_cast<void*>(&my_calloc)});
    out.push_back(Function{dll, "realloc", reinterpret_cast<void*>(&my_realloc)});
    out.push_back(Function{dll, "free", reinterpret_cast<void*>(&my_free)});
  }
}

}  // namespace am::rt::hle
