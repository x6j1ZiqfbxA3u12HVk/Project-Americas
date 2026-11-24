// Descriptor heaps. On the console a descriptor heap is memory: the game usually passes its own
// (title memory it reserved for descriptors) in the description, which is D3D12's followed by
// the memory's address and size:
//
//   +0x00 type, +0x04 count, +0x08 flags, +0x0C node mask, +0x10 address, +0x18 size in bytes
//
// and checks that the heap's CPU start is that address. Without an address the heap's memory is
// ours (zeroed). CPU and GPU handles are the same address: the console's memory is unified.
//
// The game also passes D3D12's own 16-byte description, with whatever its stack holds after it.
// So the memory fields count only when they describe the heap: a size that fits it (not much
// more), and committed memory there. Anything else gets memory of ours.

#include <windows.h>
#include <d3d12.h>

#include <cstring>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

struct HeapDescX {
  uint32_t type;
  uint32_t count;
  uint32_t flags;
  uint32_t node_mask;
  uint64_t address;
  uint64_t size;
};
static_assert(sizeof(HeapDescX) == 32);

struct DescriptorHeap : Object {
  HeapDescX desc{};
  void* owned = nullptr;  // our memory, when the game passed none
  DescriptorHeap() = default;
  DescriptorHeap(const DescriptorHeap&) = delete;
  DescriptorHeap& operator=(const DescriptorHeap&) = delete;
  ~DescriptorHeap() {
    if (owned) VirtualFree(owned, 0, MEM_RELEASE);
  }
};

const Kind& heap_kind();

// The start handles return an 8-byte handle. The game's callers take it in RAX with the heap in
// RCX; a caller passing a return buffer first is answered through the buffer.
uint64_t heap_start(uint64_t a0, uint64_t a1) {
  if (Object* o = object_of_kind(reinterpret_cast<void*>(a0), heap_kind())) {
    return static_cast<DescriptorHeap*>(o)->desc.address;
  }
  if (Object* o = object_of_kind(reinterpret_cast<void*>(a1), heap_kind())) {
    *reinterpret_cast<uint64_t*>(a0) = static_cast<DescriptorHeap*>(o)->desc.address;
    return a0;
  }
  return 0;
}

uint64_t WINAPI heap_cpu_start(uint64_t a0, uint64_t a1) { return heap_start(a0, a1); }
uint64_t WINAPI heap_gpu_start(uint64_t a0, uint64_t a1) { return heap_start(a0, a1); }

// Slot 8: GetDesc, D3D12's 16-byte description (struct return).
uint64_t WINAPI heap_get_desc(uint64_t a0, uint64_t a1) {
  Object* self = object_of_kind(reinterpret_cast<void*>(a1), heap_kind());
  uint64_t buffer = a0;
  if (!self) {
    self = object_of_kind(reinterpret_cast<void*>(a0), heap_kind());
    buffer = a1;
  }
  if (!self || !buffer) return buffer;
  std::memcpy(reinterpret_cast<void*>(buffer), &static_cast<DescriptorHeap*>(self)->desc, 16);
  return buffer;
}

const Kind& heap_kind() {
  static const Kind k("descriptor heap", 32, {
      {8, "GetDesc", reinterpret_cast<void*>(&heap_get_desc)},
      {9, "GetCPUDescriptorHandleForHeapStart", reinterpret_cast<void*>(&heap_cpu_start)},
      {10, "GetGPUDescriptorHandleForHeapStart", reinterpret_cast<void*>(&heap_gpu_start)},
  });
  return k;
}

}  // namespace

// Whether the description's memory fields name memory for this heap.
bool names_heap_memory(const HeapDescX& d, uint64_t needed) {
  if (!d.address || d.address % 16 || !needed || d.size < needed || d.size > needed + 65536) return false;
  for (const uint64_t at : {d.address, d.address + needed - 1}) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) != sizeof mbi) return false;
    // Mapped physical pages may not be committed until touched; they count.
    if (mbi.State != MEM_COMMIT && !(mbi.State == MEM_RESERVE && mbi.Type == MEM_MAPPED)) return false;
  }
  return true;
}

HRESULT create_descriptor_heap(const void* desc_in, const GUID* riid, void** out) {
  if (!out || !desc_in) return E_POINTER;
  *out = nullptr;
  HeapDescX d;
  std::memcpy(&d, desc_in, sizeof d);
  const uint64_t needed = uint64_t{d.count} * descriptor_size(d.type);
  auto* h = make_object<DescriptorHeap>(heap_kind());
  if (!names_heap_memory(d, needed)) {
    h->owned = VirtualAlloc(nullptr, needed ? needed : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!h->owned) {
      release(h);
      return E_OUTOFMEMORY;
    }
    d.address = reinterpret_cast<uint64_t>(h->owned);
    d.size = needed;
  }
  h->desc = d;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    logf("d3d12x: CreateDescriptorHeap(type %u, %u descriptors, flags %u, memory 0x%llx%s, %llu bytes, iid %s)", d.type, d.count,
         d.flags, static_cast<unsigned long long>(d.address), h->owned ? " (ours)" : "", static_cast<unsigned long long>(d.size),
         riid ? guid_text(*riid).c_str() : "null");
  }
  *out = h;
  return S_OK;
}

}  // namespace am::rt::d3d12x
