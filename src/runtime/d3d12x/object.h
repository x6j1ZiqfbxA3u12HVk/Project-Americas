#pragma once

// Console graphics (D3D12.X) objects as title code sees them: a vtable pointer at offset 0, and
// after it whatever fields the console's object has there. The console's interfaces are D3D12's
// with a different slot order and extra methods, and title code also reads and writes some
// objects' fields directly (the command list's record buffer, for one). So the title-visible
// object holds only the vtable and console-layout fields; our bookkeeping lives in a header just
// before it.
//
// Every slot not implemented yet is a probe: it logs its first calls with their arguments and
// returns 0, so a run shows which slot the game needs next and with what.
//
// Struct-returning methods: the console's callers pass the return buffer first and the object
// second (C order), so a probe also recognizes an object in the second argument.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "core/json.h"

namespace am::rt::d3d12x {

constexpr int kMaxSlots = 512;

// A Method whose function is kProbe keeps the slot a probe (for kinds without IUnknown's first
// three slots).
inline void* const kProbe = reinterpret_cast<void*>(uintptr_t{1});

struct Method {
  int slot;
  const char* name;
  void* function;
};

// The vtable and the slot names of one console interface.
class Kind {
 public:
  Kind(const char* name, int slots, std::vector<Method> methods);
  const char* name() const { return name_; }
  void* const* vtable() const { return table_.data(); }
  int slots() const { return static_cast<int>(table_.size()); }
  const char* slot_name(int slot) const;
  void count_missing(int slot);
  uint32_t missing_calls(int slot) const;

 private:
  const char* name_;
  std::vector<void*> table_;
  std::vector<const char*> names_;
  std::unique_ptr<std::atomic<uint32_t>[]> missing_;
};

// The title's view of an object. Types built on it must have no virtual functions and no other
// base: offset 0 is the console vtable.
struct Object {
  void* const* vtable;
};

// Our bookkeeping, in front of every object: 64 bytes, allocated 64-byte aligned, so the object
// is 64-byte aligned too.
constexpr size_t kObjectAlignment = 64;
struct Header {
  const Kind* kind;
  std::atomic<long> refs{1};
  void (*destroy)(Object*);
  uint8_t reserved[kObjectAlignment - 3 * sizeof(void*)];
};

inline Header* header(const Object* o) {
  return reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(o) - sizeof(Header));
}
inline const Kind* kind_of(const Object* o) { return header(o)->kind; }

// Objects are registered while alive, so a probe can tell an object from a return buffer.
void register_object(Object* o);
void unregister_object(Object* o);
bool is_object(const void* p);
// `p` as an object of `kind`, or nullptr.
Object* object_of_kind(const void* p, const Kind& kind);

// A new T (built on Object) with its header; T's constructor receives the kind's vtable.
template <class T, class... Args>
T* make_object(const Kind& kind, Args&&... args) {
  void* block = ::operator new(sizeof(Header) + sizeof(T), std::align_val_t{kObjectAlignment});
  auto* h = new (block) Header{&kind, {1}, nullptr, {}};
  T* o = new (static_cast<uint8_t*>(block) + sizeof(Header)) T(static_cast<Args&&>(args)...);
  o->vtable = kind.vtable();
  h->destroy = [](Object* p) {
    static_cast<T*>(p)->~T();
    Header* hd = header(p);
    hd->~Header();
    ::operator delete(hd, std::align_val_t{kObjectAlignment});
  };
  register_object(o);
  return o;
}

// Struct-returning methods: the console's callers pass (return buffer, object, ...). Accept the
// C++ order (object, return buffer, ...) too.
template <class T>
T* struct_return_args(uint64_t a0, uint64_t a1, Object*& self) {
  if (is_object(reinterpret_cast<void*>(a1))) {
    self = reinterpret_cast<Object*>(a1);
    return reinterpret_cast<T*>(a0);
  }
  self = reinterpret_cast<Object*>(a0);
  return reinterpret_cast<T*>(a1);
}

// Every kind adds itself here for the run report.
void register_kind(Kind* k);
void write_graphics_report(JsonWriter& j);

// QueryInterface / AddRef / Release, shared by every kind (slots 0-2).
HRESULT WINAPI query_interface(Object* self, const GUID* iid, void** out);
ULONG WINAPI add_ref(Object* self);
ULONG WINAPI release(Object* self);

// Implemented slots log their first calls this way, so a wrong guess about a slot shows.
void trace(const Object* self, int slot, const char* what, uint64_t a, uint64_t b, uint64_t c);

std::string guid_text(const GUID& g);

}  // namespace am::rt::d3d12x
