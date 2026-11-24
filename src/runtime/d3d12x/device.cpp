// The D3D12.X device. Created by d3d12_xs!D3D12XboxCreateDevice; its slots are learned from the
// game's calls (object.h). Implemented so far: what the game calls before it creates anything.

#include <windows.h>
#include <d3d12.h>

#include <cstring>

#include "runtime/d3d12x/object.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

// The game asks for this interface (read from its call of D3D12XboxCreateDevice).
constexpr GUID kDeviceIid = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

struct Device : Object {
  uint32_t params[20] = {};  // the creation parameters, as passed
  Object* dxgi = nullptr;    // the DXGI view, made on first request
};

// Slot 0: D3D12 interfaces answer with the device itself; DXGI's with the device's DXGI view.
HRESULT WINAPI device_query_interface(Device* self, const GUID* iid, void** out) {
  if (!out) return E_POINTER;
  if (iid && is_dxgi_device_iid(*iid)) {
    if (!self->dxgi) self->dxgi = create_dxgi_device(self);
    add_ref(self->dxgi);
    *out = self->dxgi;
    return S_OK;
  }
  return query_interface(self, iid, out);
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

// Slot 26: GetCustomHeapProperties(node mask, heap type). The console's memory is unified: the
// answers are D3D12's for a UMA adapter.
D3D12_HEAP_PROPERTIES* WINAPI get_custom_heap_properties(uint64_t a0, uint64_t a1, UINT node_mask,
                                                         D3D12_HEAP_TYPE type) {
  Object* self = nullptr;
  auto* out = struct_return_args<D3D12_HEAP_PROPERTIES>(a0, a1, self);
  D3D12_HEAP_PROPERTIES p{};
  p.Type = D3D12_HEAP_TYPE_CUSTOM;
  p.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
  p.CreationNodeMask = node_mask ? node_mask : 1;
  p.VisibleNodeMask = p.CreationNodeMask;
  switch (type) {
    case D3D12_HEAP_TYPE_UPLOAD: p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; break;
    case D3D12_HEAP_TYPE_READBACK: p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; break;
    default: p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; break;
  }
  std::memcpy(out, &p, sizeof p);
  return out;
}

// Slot 8: CreateCommandQueue(desc, riid, out), as in D3D12.
HRESULT WINAPI create_command_queue(Device*, const void* desc, const GUID* riid, void** out) {
  return create_queue(desc, 16, riid, out, "CreateCommandQueue");
}

// Slot 36: CreateFence(initial value, flags, riid, out), as in D3D12.
HRESULT WINAPI create_fence_slot(Device*, UINT64 initial, UINT flags, const GUID* riid, void** out) {
  return create_fence(initial, flags, riid, out);
}

// Slot 48: the console's own queue creation; the game asks for the same interface as from
// CreateCommandQueue, with a larger description.
HRESULT WINAPI create_command_queue_x(Device*, const void* desc, const GUID* riid, void** out) {
  return create_queue(desc, 24, riid, out, "CreateCommandQueueX (slot 48)");
}

// Slot 9: CreateCommandAllocator(type, riid, out), as in D3D12.
HRESULT WINAPI create_command_allocator(Device*, UINT type, const GUID* riid, void** out) {
  return create_allocator(type, riid, out);
}

// Slot 73: the console's command list creation: (16-byte description, allocator, initial
// pipeline, riid, out).
HRESULT WINAPI create_command_list_x(Device*, const uint32_t* desc, Object* allocator, Object* pipeline,
                                     const GUID* riid, void** out) {
  return create_command_list(desc, allocator, pipeline, riid, out);
}

// Slot 15: GetDescriptorHandleIncrementSize(type), as in D3D12 (sizes: descriptor_size).
UINT WINAPI get_descriptor_handle_increment_size(Device* self, UINT type) {
  trace(self, 15, "GetDescriptorHandleIncrementSize", type, 0, 0);
  return descriptor_size(type);
}

// Slot 14: CreateDescriptorHeap(console description, riid, out).
HRESULT WINAPI create_descriptor_heap_slot(Device*, const void* desc, const GUID* riid, void** out) {
  return create_descriptor_heap(desc, riid, out);
}

std::vector<Method> device_methods() {
  std::vector<Method> m = {
      {0, "QueryInterface", reinterpret_cast<void*>(&device_query_interface)},
      {8, "CreateCommandQueue", reinterpret_cast<void*>(&create_command_queue)},
      {9, "CreateCommandAllocator", reinterpret_cast<void*>(&create_command_allocator)},
      {14, "CreateDescriptorHeap", reinterpret_cast<void*>(&create_descriptor_heap_slot)},
      {15, "GetDescriptorHandleIncrementSize", reinterpret_cast<void*>(&get_descriptor_handle_increment_size)},
      {26, "GetCustomHeapProperties", reinterpret_cast<void*>(&get_custom_heap_properties)},
      {36, "CreateFence", reinterpret_cast<void*>(&create_fence_slot)},
      {48, "CreateCommandQueueX", reinterpret_cast<void*>(&create_command_queue_x)},
      {73, "CreateCommandListX", reinterpret_cast<void*>(&create_command_list_x)},
  };
  add_view_methods(m);
  return m;
}

const Kind& device_kind() {
  static const Kind k("device", 256, device_methods());
  return k;
}

}  // namespace

// The game sizes its descriptor heaps in title memory with these. Resource descriptors on the
// console's GPU are 32 bytes for images (T#) and 16 for buffers (V#): a heap that holds both
// needs 32-byte slots. Samplers (S#) are 16. Render-target and depth descriptors exist only for
// our own use, so their size is ours to choose. (To confirm against the descriptor-table strides
// in the console's shaders.)
uint32_t descriptor_size(uint32_t type) {
  switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return 32;
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return 16;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return 64;
    default: return 32;  // the console's own heap type 4
  }
}

HRESULT WINAPI create_device(void* adapter, const uint32_t* params, const GUID* riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  auto* d = make_object<Device>(device_kind());
  if (params) std::memcpy(d->params, params, sizeof d->params);
  logf("D3D12XboxCreateDevice(adapter %p, iid %s)", adapter, riid ? guid_text(*riid).c_str() : "null");
  logf("  parameters: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
       d->params[0], d->params[1], d->params[2], d->params[3], d->params[4], d->params[5], d->params[6], d->params[7],
       d->params[8], d->params[9], d->params[10], d->params[11], d->params[12], d->params[13], d->params[14],
       d->params[15], d->params[16], d->params[17], d->params[18], d->params[19]);
  if (riid && !IsEqualGUID(*riid, kDeviceIid)) logf("  (an interface we have not seen the game ask for)");
  *out = d;
  return S_OK;
}

}  // namespace am::rt::d3d12x
