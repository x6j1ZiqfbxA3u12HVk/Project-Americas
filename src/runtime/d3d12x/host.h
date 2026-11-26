#pragma once

// Host GPU resources for the console's resources (plan, 5.2). A resource the GPU produces
// (render target, depth, storage texture) lives on the GPU only: it gets a host texture with
// its description the first time a command needs it, and its views get host descriptors.
// Resources the CPU produces (streamed textures, buffers) come with the binding model.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace am::rt::d3d12x {

// The host texture of a console resource made for the GPU to write (render target, depth or
// storage flags); made on first use when `create`. nullptr for other resources, or without a
// host GPU.
ID3D12Resource* host_texture(const void* console_resource, bool create = true);
// Host render-target and depth views (format 0: the resource's own, made typed for depth).
// ptr 0 when none can be made.
D3D12_CPU_DESCRIPTOR_HANDLE host_rtv(ID3D12Resource* texture, uint32_t format);
D3D12_CPU_DESCRIPTOR_HANDLE host_dsv(ID3D12Resource* texture, uint32_t format);
// The formats host_rtv and host_dsv give a texture's views.
DXGI_FORMAT host_rtv_format(ID3D12Resource* texture, uint32_t format);
DXGI_FORMAT host_dsv_format(ID3D12Resource* texture, uint32_t format);
// Drops a console resource's host texture and its views (the console resource is going away).
void forget_host_texture(const void* console_resource);
// The host texture of a GPU-written console resource whose memory starts at `address` (its
// description in `desc`), or nullptr.
ID3D12Resource* host_texture_at(uint64_t address, D3D12_RESOURCE_DESC* desc);

// host_pipeline.cpp: the 24-byte marker (hex) of a shader container's program of a stage (a
// stream subobject type: 1 VS, 2 PS, 6 CS); its precompiled DXIL; a host graphics pipeline
// from a description (the translated root signature is filled in), or nullptr.
std::string program_marker(std::span<const uint8_t> container, uint32_t subobject);
bool load_precompiled(const std::string& marker, std::vector<uint8_t>& dxil);
// A program a draw or dispatch waited for: listed in rt<N>\wanted-first.txt, which the next
// `am-shaders precompile` compiles before everything else.
void note_wanted(const std::string& marker);
ID3D12PipelineState* make_host_graphics_pipeline(D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, const char* what);

// pipeline.cpp: a console pipeline's host pipeline (nullptr when there is none) and its console
// root signature object. The console's own pipelines (slot 57) are made at their first draw, for
// the render targets bound then.
// `rect_list`: drawn with the console's rect list (topology 17), which the host draws as triangles
// with a geometry shader adding each rectangle's fourth corner.
ID3D12PipelineState* host_pipeline(const void* console_pipeline, const void** root_signature, const DXGI_FORMAT* targets,
                                   uint32_t target_count, DXGI_FORMAT depth, bool rect_list);

// A console compute pipeline's host pipeline (nullptr when it is not a compute pipeline or its
// program is not precompiled) and its console root signature object.
ID3D12PipelineState* host_compute_pipeline(const void* console_pipeline, const void** root_signature);
// Where a console compute pipeline's root parameters arrive, from its program's metadata: per
// root parameter the user-data dword it starts at (s<user> for compute), its root type (0
// constants, 1 root descriptor, 3 table, 5 / 6 heap addresses) and descriptor dwords. nullptr
// when the pipeline has no such records (the packed order from s8 applies).
struct UserSlot {
  uint8_t root, user, slot, dwords;
};
const std::vector<UserSlot>* compute_user_layout(const void* console_pipeline);
// "N made, M not precompiled, ... of K queued", for the frame log.
std::string compute_pipeline_counts();

}  // namespace am::rt::d3d12x
