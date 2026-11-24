#pragma once

// d3d12_xs.dll's entry points (plan, section 5.5).

#include <windows.h>

#include <cstdint>
#include <vector>

namespace am::rt::d3d12x {

// D3D12XboxCreateDevice(adapter, parameters, riid, device): the adapter is null in the game's
// call; the parameters are an 80-byte block.
HRESULT WINAPI create_device(void* adapter, const uint32_t* params, const GUID* riid, void** out);

// Objects the device creates (queue.cpp). `how` names the creating method in the log.
HRESULT create_queue(const void* desc, size_t desc_size, const GUID* riid, void** out, const char* how);
HRESULT create_fence(uint64_t initial, uint32_t flags, const GUID* riid, void** out);

struct Object;
// dxgi.cpp: the device's DXGI view (IDXGIDevice, with its adapter and output).
bool is_dxgi_device_iid(const GUID& iid);
Object* create_dxgi_device(Object* device);

// device.cpp: bytes per descriptor of a heap type, as the device reports them.
uint32_t descriptor_size(uint32_t type);

struct Method;
// views.cpp: device slots 17-25 (views, descriptor copies, GetResourceAllocationInfo).
void add_view_methods(std::vector<Method>& out);

// descriptor_heap.cpp: the console's description (D3D12's, then memory address and size).
HRESULT create_descriptor_heap(const void* desc, const GUID* riid, void** out);

// list.cpp
HRESULT create_allocator(uint32_t type, const GUID* riid, void** out);
HRESULT create_command_list(const uint32_t* desc, Object* allocator, Object* pipeline, const GUID* riid, void** out);

}  // namespace am::rt::d3d12x
