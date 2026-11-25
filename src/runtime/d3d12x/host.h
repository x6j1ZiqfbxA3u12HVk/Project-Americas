#pragma once

// Host GPU resources for the console's resources (plan, 5.2). A resource the GPU produces
// (render target, depth, storage texture) lives on the GPU only: it gets a host texture with
// its description the first time a command needs it, and its views get host descriptors.
// Resources the CPU produces (streamed textures, buffers) come with the binding model.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace am::rt::d3d12x {

// The host texture of a console resource made for the GPU to write (render target, depth or
// storage flags); made on first use when `create`. nullptr for other resources, or without a
// host GPU.
ID3D12Resource* host_texture(const void* console_resource, bool create = true);
// Host render-target and depth views (format 0: the resource's own, made typed for depth).
// ptr 0 when none can be made.
D3D12_CPU_DESCRIPTOR_HANDLE host_rtv(ID3D12Resource* texture, uint32_t format);
D3D12_CPU_DESCRIPTOR_HANDLE host_dsv(ID3D12Resource* texture, uint32_t format);
// Drops a console resource's host texture and its views (the console resource is going away).
void forget_host_texture(const void* console_resource);

}  // namespace am::rt::d3d12x
