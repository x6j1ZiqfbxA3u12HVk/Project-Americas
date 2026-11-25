#pragma once

// Images and samplers for translated shaders (shader/image_bindings.h). A console shader reads
// a texture's descriptor (T#, 8 words) and a sampler's (S#, 4 words) from memory or builds them
// in registers; the translated shader looks those words up in a table the runtime keeps, which
// gives it host descriptor heap indices, and queues the words it does not find.
//
// * Texture views made through our API (slot 70) write a real T# into the title's descriptor
//   and add it to the table, with a host texture whose data comes from the title's memory
//   (laid out as layout.h says), uploaded again when the title's memory there is written
//   through a path we see (DirectStorage, XG).
// * Samplers arrive as misses (shaders build most S# from constants): decoded and added.
// * At each submission the table is rebuilt if it changed, the textures that changed are
//   uploaded, and the misses the GPU queued are read back for the next frame.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace am::rt::d3d12x {

// A texture view's T# (written to `words`), its host texture and its table entry. With `host`
// (a texture the GPU renders into), the entry views that texture: nothing is uploaded.
void bind_texture_view(const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view, uint64_t address,
                       uint32_t words[8], ID3D12Resource* host = nullptr);
// The title's memory in [address, address + size) was written: textures over it are uploaded
// again at the next submission.
void notify_title_write(uint64_t address, uint64_t size);
// Records the uploads and the table's upkeep into `list`, before its draws.
void flush_bindings(ID3D12GraphicsCommandList* list);
// The lists holding the last flush's readback were submitted; `fence_value` follows them.
void bindings_submitted(uint64_t fence_value);
// The table (root UAV u0 of translated programs) and the shader-visible sampler heap.
D3D12_GPU_VIRTUAL_ADDRESS bindings_table();
ID3D12DescriptorHeap* sampler_heap();

// An RDNA 2 image descriptor's GPU format for a DXGI format (0 when we have none).
uint32_t image_format(uint32_t dxgi_format);

}  // namespace am::rt::d3d12x
