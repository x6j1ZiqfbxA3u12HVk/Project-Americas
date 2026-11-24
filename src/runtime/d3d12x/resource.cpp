// Resources. Memory is unified on the console: a resource's GPU virtual address is its CPU
// address, and its memory comes from XMemAlloc with the graphics runtime's allocator id, so the
// title's allocation hooks place it (the game places its back buffers in a budget of its own and
// checks that they filled it).
//
// The resource's slots follow D3D12's ID3D12Resource order (to confirm by the game's calls; each
// implemented slot traces its first calls).

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

#include "core/text.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/hle/xmem.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

namespace xalloc = hle::xalloc;

struct Resource : Object {
  D3D12_RESOURCE_DESC desc{};
  D3D12_HEAP_PROPERTIES heap{};
  uint32_t heap_flags = 0;
  uint32_t initial_layout = 0;  // a D3D12_BARRIER_LAYOUT
  uint64_t address = 0;     // CPU and GPU address
  uint64_t size = 0;        // bytes reserved for it
  uint64_t attributes = 0;  // XMemAlloc's, for the free
  Object* device = nullptr;

  ~Resource() {
    if (address) hle::xmem_free(reinterpret_cast<void*>(address), attributes);
  }
};

// Slot 7: GetDevice(riid, out).
HRESULT WINAPI get_device(Resource* self, const GUID* iid, void** out) {
  trace(self, 7, "GetDevice", 0, 0, 0);
  if (!out) return E_POINTER;
  if (!self->device) {
    *out = nullptr;
    return E_FAIL;
  }
  return query_interface(self->device, iid, out);
}

// Slot 8: Map(subresource, read range, data). Every resource is CPU-visible here; textures map
// at their start (subresource offsets wait for the console's texture layouts).
HRESULT WINAPI map(Resource* self, UINT subresource, const D3D12_RANGE*, void** data) {
  trace(self, 8, "Map", subresource, reinterpret_cast<uint64_t>(data), 0);
  if (data) *data = reinterpret_cast<void*>(self->address);
  return S_OK;
}

// Slot 9: Unmap(subresource, written range): nothing to do on unified memory.
void WINAPI unmap(Resource* self, UINT subresource, const D3D12_RANGE*) { trace(self, 9, "Unmap", subresource, 0, 0); }

// Slot 10: GetDesc (struct return).
D3D12_RESOURCE_DESC* WINAPI get_desc(uint64_t a0, uint64_t a1) {
  Object* self = nullptr;
  auto* out = struct_return_args<D3D12_RESOURCE_DESC>(a0, a1, self);
  trace(self, 10, "GetDesc", 0, 0, 0);
  std::memcpy(out, &static_cast<Resource*>(self)->desc, sizeof(D3D12_RESOURCE_DESC));
  return out;
}

// Slot 11: GetGPUVirtualAddress. D3D12 gives 0 for textures; the console's textures have
// addresses too, and the title's shaders read them from descriptors it writes itself.
uint64_t WINAPI get_gpu_virtual_address(Resource* self) {
  trace(self, 11, "GetGPUVirtualAddress", self->address, 0, 0);
  return self->address;
}

// Slot 14: GetHeapProperties(properties, flags).
HRESULT WINAPI get_heap_properties(Resource* self, D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS* flags) {
  trace(self, 14, "GetHeapProperties", 0, 0, 0);
  if (props) *props = self->heap;
  if (flags) *flags = static_cast<D3D12_HEAP_FLAGS>(self->heap_flags);
  return S_OK;
}

const Kind& resource_kind() {
  static const Kind k("resource", 64,
                      {
                          {7, "GetDevice", reinterpret_cast<void*>(&get_device)},
                          {8, "Map", reinterpret_cast<void*>(&map)},
                          {9, "Unmap", reinterpret_cast<void*>(&unmap)},
                          {10, "GetDesc", reinterpret_cast<void*>(&get_desc)},
                          {11, "GetGPUVirtualAddress", reinterpret_cast<void*>(&get_gpu_virtual_address)},
                          {14, "GetHeapProperties", reinterpret_cast<void*>(&get_heap_properties)},
                      });
  return k;
}

}  // namespace

// Large pages for resources of 2 MiB and more (the game's back-buffer budget takes only those),
// 64 KiB pages for the rest unless they need no more than 4 KiB alignment. Memory type 0: no
// handler on the path reads it.
uint64_t resource_attributes(uint64_t size, uint64_t alignment) {
  constexpr uint64_t k2M = 2u << 20, k64K = 64u << 10;
  const uint64_t page = size >= k2M ? xalloc::kPageLarge : alignment >= k64K ? xalloc::kPage64K : xalloc::kPage4K;
  const uint32_t shift = static_cast<uint32_t>(std::countr_zero(std::max<uint64_t>(std::bit_ceil(alignment), 16)));
  return xalloc::attributes(page, xalloc::kAllocatorD3D, shift);
}

HRESULT create_committed_resource(Object* device, const D3D12_HEAP_PROPERTIES* heap, uint32_t heap_flags,
                                  const D3D12_RESOURCE_DESC* desc, uint32_t initial_layout,
                                  const D3D12_CLEAR_VALUE* /*clear*/, const GUID* /*riid*/, void** out) {
  if (!desc || !heap) return E_INVALIDARG;
  const D3D12_RESOURCE_ALLOCATION_INFO info = allocation_info(*desc);
  if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes) return E_INVALIDARG;
  if (!out) return S_FALSE;  // D3D12: a null out pointer only checks the arguments
  *out = nullptr;
  const uint64_t alignment = std::max<uint64_t>(info.Alignment, desc->Alignment);
  const uint64_t attributes = resource_attributes(info.SizeInBytes, alignment);
  void* memory = hle::xmem_alloc(info.SizeInBytes, attributes);
  if (!memory) {
    logf("d3d12x: no memory for a resource of %s", format_bytes(info.SizeInBytes).c_str());
    return E_OUTOFMEMORY;
  }
  // D3D12 hands out committed resources zeroed unless asked not to.
  if (!(heap_flags & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED)) std::memset(memory, 0, info.SizeInBytes);

  auto* r = make_object<Resource>(resource_kind());
  r->desc = *desc;
  r->heap = *heap;
  r->heap_flags = heap_flags;
  r->initial_layout = initial_layout;
  r->address = reinterpret_cast<uint64_t>(memory);
  r->size = info.SizeInBytes;
  r->attributes = attributes;
  r->device = device;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
    logf("d3d12x: committed resource (dim %u, %llux%u x%u, %u mips, format %u, flags 0x%x; heap %u, flags 0x%x, layout %u)"
         " = %p: %s at 0x%s, attributes 0x%s",
         desc->Dimension, static_cast<unsigned long long>(desc->Width), desc->Height, desc->DepthOrArraySize,
         desc->MipLevels, desc->Format, desc->Flags, heap->Type, heap_flags, initial_layout, static_cast<void*>(r),
         format_bytes(r->size).c_str(), hex(r->address).c_str(), hex(attributes).c_str());
  }
  *out = r;
  return S_OK;
}

}  // namespace am::rt::d3d12x
