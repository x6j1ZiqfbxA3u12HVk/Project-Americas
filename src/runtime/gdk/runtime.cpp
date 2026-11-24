#include "runtime/gdk/runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "core/text.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::gdk {
namespace {

constexpr int kMaxSlots = 96;

struct Slot {
  const char* name = nullptr;
  std::atomic<uint32_t> calls{0};  // calls of a missing method
};

// A runtime object as title code sees it: the vtable pointer first.
struct Object {
  void* const* vtable = nullptr;
  const Interface* iface = nullptr;
  std::vector<void*> table;
  std::unique_ptr<Slot[]> slots;
  std::atomic<uint64_t> queries{0};
  std::atomic<bool> announced{false};
  bool offered = false;
};

std::string text(const GUID& g) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

HRESULT WINAPI query_interface(Object* self, const GUID* iid, void** out) {
  if (!out) return E_POINTER;
  if (iid && IsEqualGUID(*iid, self->iface->iid)) {
    *out = self;
    return S_OK;
  }
  *out = nullptr;
  return E_NOINTERFACE;
}
ULONG WINAPI add_ref_or_release(Object*) { return 1; }  // the objects are static

void report_missing(Object* self, int slot) {
  Slot& s = self->slots[slot];
  if (s.calls.fetch_add(1, std::memory_order_relaxed) == 0) {
    logf("runtime: %s slot %d (%s) is not implemented", self->iface->area, slot, s.name ? s.name : "?");
  }
}

template <int N>
HRESULT WINAPI missing_hresult(Object* self) {
  report_missing(self, N);
  return E_NOTIMPL;
}
template <int N>
uint64_t WINAPI missing_zero(Object* self) {
  report_missing(self, N);
  return 0;
}

template <int... N>
std::array<void*, sizeof...(N)> hresult_stubs(std::integer_sequence<int, N...>) {
  return {reinterpret_cast<void*>(&missing_hresult<N>)...};
}
template <int... N>
std::array<void*, sizeof...(N)> zero_stubs(std::integer_sequence<int, N...>) {
  return {reinterpret_cast<void*>(&missing_zero<N>)...};
}

struct Catalog {
  std::vector<Interface> interfaces;
  std::vector<std::unique_ptr<Object>> objects;
  std::mutex unknown_lock;
  std::vector<std::pair<GUID, GUID>> unknown;  // asked for, not in the catalog
};

Catalog& catalog() {
  static Catalog* const c = [] {
    auto* cat = new Catalog;
    add_task_queue_interfaces(cat->interfaces);
    add_offline_interfaces(cat->interfaces);
    add_system_interfaces(cat->interfaces);
    static const auto hresult = hresult_stubs(std::make_integer_sequence<int, kMaxSlots>());
    static const auto zero = zero_stubs(std::make_integer_sequence<int, kMaxSlots>());
    for (const Interface& i : cat->interfaces) {
      auto o = std::make_unique<Object>();
      int top = 3;
      for (const Method& m : i.methods) top = std::max(top, m.slot + 1);
      top = std::min(top, kMaxSlots);
      o->table.resize(top);
      o->slots = std::make_unique<Slot[]>(top);
      o->table[0] = reinterpret_cast<void*>(&query_interface);
      o->table[1] = reinterpret_cast<void*>(&add_ref_or_release);
      o->table[2] = reinterpret_cast<void*>(&add_ref_or_release);
      for (int s = 3; s < top; ++s) o->table[s] = hresult[s];
      for (const Method& m : i.methods) {
        if (m.slot < 3 || m.slot >= top) continue;
        o->slots[m.slot].name = m.name;
        if (m.function) {
          o->table[m.slot] = m.function;
          o->offered = true;
        } else {
          o->table[m.slot] = m.returns == Returns::Zero ? zero[m.slot] : hresult[m.slot];
        }
      }
      o->vtable = o->table.data();
      o->iface = &i;
      cat->objects.push_back(std::move(o));
    }
    return cat;
  }();
  return *c;
}

// ---- xgameruntime.dll's exports ----

HRESULT WINAPI my_InitializeApiImpl(uint64_t version, uint64_t build) {
  logf("runtime: InitializeApiImpl(0x%s, 0x%s)", hex(version).c_str(), hex(build).c_str());
  return S_OK;
}
HRESULT WINAPI my_InitializeApiImplEx(uint64_t version, uint64_t build, uint64_t flags) {
  logf("runtime: InitializeApiImplEx(0x%s, 0x%s, 0x%s)", hex(version).c_str(), hex(build).c_str(), hex(flags).c_str());
  return S_OK;
}
// The title's GDK passes its version and build, flags, and its initialization options (null
// unless it initializes with options).
HRESULT WINAPI my_InitializeApiImplEx2(uint64_t version, uint64_t build, uint64_t flags, const void* options) {
  logf("runtime: InitializeApiImplEx2(0x%s, 0x%s, 0x%s, %p)", hex(version).c_str(), hex(build).c_str(),
       hex(flags).c_str(), options);
  return S_OK;
}
void WINAPI my_UninitializeApiImpl() { logf("runtime: UninitializeApiImpl"); }
HRESULT WINAPI my_QueryApiImpl(const GUID* cls, const GUID* iid, void** out) { return query_api(cls, iid, out); }
void WINAPI my_XErrorReport(HRESULT hr, const char* message) {
  logf("runtime: XErrorReport(0x%08lx, %s)", static_cast<unsigned long>(hr), message ? message : "");
}

}  // namespace

HRESULT query_api(const GUID* cls, const GUID* iid, void** out) {
  if (!cls || !iid || !out) return E_INVALIDARG;
  *out = nullptr;
  Catalog& c = catalog();
  for (const auto& o : c.objects) {
    if (!IsEqualGUID(*cls, o->iface->cls) || !IsEqualGUID(*iid, o->iface->iid)) continue;
    o->queries.fetch_add(1, std::memory_order_relaxed);
    if (o->offered) {
      *out = o.get();
      return S_OK;
    }
    if (!o->announced.exchange(true)) logf("runtime: %s is not implemented yet", o->iface->area);
    return E_NOINTERFACE;
  }
  std::lock_guard<std::mutex> lock(c.unknown_lock);
  const auto same = [&](const std::pair<GUID, GUID>& p) { return IsEqualGUID(p.first, *cls) && IsEqualGUID(p.second, *iid); };
  if (std::none_of(c.unknown.begin(), c.unknown.end(), same)) {
    c.unknown.emplace_back(*cls, *iid);
    logf("runtime: unknown interface %s / %s", text(*cls).c_str(), text(*iid).c_str());
  }
  return E_NOINTERFACE;
}

void add_runtime_exports(std::vector<hle::Function>& out) {
#define AM_RUNTIME(name) hle::Function{"xgameruntime.dll", #name, reinterpret_cast<void*>(&my_##name)}
  out.insert(out.end(), {AM_RUNTIME(InitializeApiImpl), AM_RUNTIME(InitializeApiImplEx), AM_RUNTIME(InitializeApiImplEx2),
                         AM_RUNTIME(UninitializeApiImpl), AM_RUNTIME(QueryApiImpl), AM_RUNTIME(XErrorReport)});
#undef AM_RUNTIME
}

void write_runtime_report(JsonWriter& j) {
  Catalog& c = catalog();
  j.begin_array();
  for (const auto& o : c.objects) {
    const uint64_t queries = o->queries.load();
    if (!queries) continue;
    j.begin_object().field("area", o->iface->area).field("iid", text(o->iface->iid)).field("queries", queries)
        .field("offered", o->offered);
    j.key("missing_called").begin_array();
    for (size_t s = 0; s < o->table.size(); ++s) {
      const uint32_t calls = o->slots[s].calls.load();
      if (!calls) continue;
      j.begin_object().field("slot", static_cast<uint64_t>(s)).field("name", o->slots[s].name ? o->slots[s].name : "?")
          .field("calls", static_cast<uint64_t>(calls)).end_object();
    }
    j.end_array().end_object();
  }
  std::lock_guard<std::mutex> lock(c.unknown_lock);
  for (const auto& [cls, iid] : c.unknown) {
    j.begin_object().field("area", "unknown").field("class", text(cls)).field("iid", text(iid)).end_object();
  }
  j.end_array();
}

}  // namespace am::rt::gdk
