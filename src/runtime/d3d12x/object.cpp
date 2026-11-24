#include "runtime/d3d12x/object.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include "runtime/log.h"
#include "runtime/process.h"

namespace am::rt::d3d12x {

static_assert(offsetof(Object, vtable) == 0, "title code reads the vtable at offset 0");
static_assert(sizeof(Header) == 64, "the header keeps objects 64-byte aligned");

namespace {

std::shared_mutex g_objects_lock;
std::unordered_set<const void*>& objects() {
  static auto* s = new std::unordered_set<const void*>;
  return *s;
}

std::mutex g_kinds_lock;
std::vector<Kind*>& kinds() {
  static auto* v = new std::vector<Kind*>;
  return *v;
}

std::atomic<uint64_t> g_sequence{0};

// The first calls of a slot we do not implement: logged with the integer arguments.
// "module+offset" for a code address in a title module (where a probe was called from).
std::string caller_text(const void* address) {
  char text[96];
  if (TitleProcess* p = process()) {
    if (const MappedImage* m = p->module_containing(address)) {
      std::snprintf(text, sizeof text, "%s+0x%llx", m->name.c_str(),
                    static_cast<unsigned long long>(static_cast<const uint8_t*>(address) - m->base));
      return text;
    }
  }
  std::snprintf(text, sizeof text, "%p", address);
  return text;
}

template <int N>
__declspec(noinline) uint64_t probe(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                    uint64_t a6, uint64_t a7) {
  Object* self = nullptr;
  bool struct_return = false;
  if (is_object(reinterpret_cast<void*>(a0))) {
    self = reinterpret_cast<Object*>(a0);
  } else if (is_object(reinterpret_cast<void*>(a1))) {
    self = reinterpret_cast<Object*>(a1);  // return buffer in a0
    struct_return = true;
  }
  if (!self) {
    logf("d3d12x: slot %d called on an unknown object 0x%llx", N, static_cast<unsigned long long>(a0));
    return 0;
  }
  Kind* kind = const_cast<Kind*>(kind_of(self));
  const uint32_t calls = kind->missing_calls(N);
  kind->count_missing(N);
  if (calls < 3) {
    const uint64_t seq = g_sequence.fetch_add(1);
    const char* name = kind->slot_name(N);
    logf("d3d12x: #%llu %s slot %d%s%s%s not implemented (from %s): %s%llx %llx %llx %llx %llx %llx %llx",
         static_cast<unsigned long long>(seq), kind->name(), N, name ? " (" : "", name ? name : "", name ? ")" : "",
         caller_text(_ReturnAddress()).c_str(), struct_return ? "[struct return] " : "",
         static_cast<unsigned long long>(a0), static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
         static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4), static_cast<unsigned long long>(a5),
         static_cast<unsigned long long>(a6));
  }
  (void)a7;
  return 0;
}

template <int... N>
std::array<void*, sizeof...(N)> probes(std::integer_sequence<int, N...>) {
  return {reinterpret_cast<void*>(&probe<N>)...};
}

const std::array<void*, kMaxSlots>& probe_table() {
  static const auto table = probes(std::make_integer_sequence<int, kMaxSlots>());
  return table;
}

}  // namespace

Kind::Kind(const char* name, int slots, std::vector<Method> methods) : name_(name) {
  slots = std::min(std::max(slots, 3), kMaxSlots);
  const auto& p = probe_table();
  table_.assign(p.begin(), p.begin() + slots);
  names_.assign(static_cast<size_t>(slots), nullptr);
  missing_ = std::make_unique<std::atomic<uint32_t>[]>(static_cast<size_t>(slots));
  table_[0] = reinterpret_cast<void*>(&query_interface);
  table_[1] = reinterpret_cast<void*>(&add_ref);
  table_[2] = reinterpret_cast<void*>(&release);
  names_[0] = "QueryInterface";
  names_[1] = "AddRef";
  names_[2] = "Release";
  for (const Method& m : methods) {
    if (m.slot < 0 || m.slot >= slots) continue;
    names_[static_cast<size_t>(m.slot)] = m.name;
    if (m.function == kProbe) table_[static_cast<size_t>(m.slot)] = p[static_cast<size_t>(m.slot)];
    else if (m.function) table_[static_cast<size_t>(m.slot)] = m.function;
  }
  register_kind(this);
}

const char* Kind::slot_name(int slot) const {
  return slot >= 0 && slot < slots() ? names_[static_cast<size_t>(slot)] : nullptr;
}

void Kind::count_missing(int slot) {
  if (slot >= 0 && slot < slots()) missing_[static_cast<size_t>(slot)].fetch_add(1, std::memory_order_relaxed);
}

uint32_t Kind::missing_calls(int slot) const {
  return slot >= 0 && slot < slots() ? missing_[static_cast<size_t>(slot)].load(std::memory_order_relaxed) : 0;
}

void register_object(Object* o) {
  std::unique_lock lock(g_objects_lock);
  objects().insert(o);
}

void unregister_object(Object* o) {
  std::unique_lock lock(g_objects_lock);
  objects().erase(o);
}

bool is_object(const void* p) {
  if (!p) return false;
  std::shared_lock lock(g_objects_lock);
  return objects().count(p) != 0;
}

Object* object_of_kind(const void* p, const Kind& kind) {
  if (!is_object(p)) return nullptr;
  auto* o = static_cast<Object*>(const_cast<void*>(p));
  return kind_of(o) == &kind ? o : nullptr;
}

void register_kind(Kind* k) {
  std::lock_guard lock(g_kinds_lock);
  kinds().push_back(k);
}

void write_graphics_report(JsonWriter& j) {
  std::lock_guard lock(g_kinds_lock);
  j.key("graphics").begin_array();
  for (const Kind* k : kinds()) {
    for (int s = 3; s < k->slots(); ++s) {
      const uint32_t n = k->missing_calls(s);
      if (!n) continue;
      j.begin_object().field("interface", k->name()).field("slot", static_cast<int64_t>(s))
          .field("name", k->slot_name(s) ? k->slot_name(s) : "?").field("calls", n).end_object();
    }
  }
  j.end_array();
}

std::string guid_text(const GUID& g) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return buf;
}

void trace(const Object* self, int slot, const char* what, uint64_t a, uint64_t b, uint64_t c) {
  static std::atomic<int> budget{300};
  if (budget.fetch_sub(1) > 0) {
    logf("d3d12x: %s slot %d %s(%llx, %llx, %llx)", kind_of(self)->name(), slot, what,
         static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(c));
  }
}

HRESULT WINAPI query_interface(Object* self, const GUID* iid, void** out) {
  if (!out) return E_POINTER;
  // One object answers for every version of its interface, as the console's do.
  static std::atomic<int> logged{0};
  if (iid && logged.fetch_add(1) < 32) {
    logf("d3d12x: %s QueryInterface(%s)", kind_of(self)->name(), guid_text(*iid).c_str());
  }
  add_ref(self);
  *out = self;
  return S_OK;
}

ULONG WINAPI add_ref(Object* self) { return static_cast<ULONG>(header(self)->refs.fetch_add(1) + 1); }

ULONG WINAPI release(Object* self) {
  Header* h = header(self);
  const long n = h->refs.fetch_sub(1) - 1;
  if (n == 0) {
    unregister_object(self);
    h->destroy(self);
  }
  return static_cast<ULONG>(n < 0 ? 0 : n);
}

}  // namespace am::rt::d3d12x
