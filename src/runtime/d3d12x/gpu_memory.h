#pragma once

// The title's memory as host GPU memory (plan 5.2, class C): translated shaders read and write
// console GPU addresses in place, zero copy.
//
// Each title allocation the GPU touches is opened as a host heap over its own pages
// (OpenExistingHeapFromAddress) with one buffer over all of it: a "window", whose raw view sits
// in the shader-visible heap. A page table maps every 64 KiB of the console's address space,
// [0, 2^40), to its window's view and its page in the window (window << 16 | page; 0: none).
// The translator's runtime binding reads it (shader/translate.cpp, kRuntimeBinding):
//
//   shader heap [0]      the page table (raw, 64 MiB)
//   shader heap [1 + n]  window n (raw, writable), from the bottom up
//   shader heap [top]    texture views (bindings.cpp), from the top down
//
// Physical pages come from two pools (memory/title_memory.h). The committed pool (the game's
// small batches: descriptors, per-frame data) is one window too; the title memory reports every
// view of its pages and the page table follows. The pool committed on touch (the streaming
// heap) cannot be opened in place: its pages get VRAM copies instead (to come).
//
// Translated programs use one root signature: 20 root constants at b0 (the user data SGPRs) and
// the image binding table as root UAV u0 (bindings.h); views from the shader heap by index.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace am::rt::d3d12x {

constexpr uint32_t kPageTableIndex = 0;
constexpr uint32_t kUserDataConstants = 20;

// Opens the window of the title allocation holding `address` (if it is not open yet); false
// when the address is not in one we can open.
bool open_gpu_window(uint64_t address);
// The shader-visible heap and the root signature of translated programs; nullptr without a
// host GPU.
ID3D12DescriptorHeap* translated_shader_heap();
ID3D12RootSignature* translated_root_signature();
// Compute: user data at root parameter 0, the binding table at 1. Graphics: the vertex stage's
// user data at root parameter 0, the pixel stage's at 1 (both b0, 20 constants, each visible to
// its stage), the binding table at 2.
ID3D12RootSignature* translated_graphics_root_signature();
// The host GPU address of title memory [address, address + size): in a window of its own
// allocation, or in the committed page pool where its pages lie in a row there; 0 when neither
// (vertex and index buffers bind it in place).
D3D12_GPU_VIRTUAL_ADDRESS host_gpu_address(uint64_t address, uint64_t size);
// A shader heap index for a texture view (and its CPU handle); 0 when the heap is full.
uint32_t allocate_view_slot(D3D12_CPU_DESCRIPTOR_HANDLE* handle);
// Gives a slot back (once no submitted work reads it): the next allocation takes it.
void release_view_slot(uint32_t index);
// Records the page table's pending updates into `list` (before work that reads them).
void flush_page_table(ID3D12GraphicsCommandList* list);
// `list` (one that flushed) was submitted; `fence_value` follows it: its uploads are freed once
// that completes.
void page_table_submitted(uint64_t fence_value, const void* list);
// The GPU's own page mapping (queue slot 31): the 64 KiB pages from `address` read GPU pool
// pages `pages` (-1: unmapped or not in the committed pool), whatever the CPU sees there. On
// the CPU side their bytes are at `source` (title memory, the pages' view; 0: nowhere), a page
// further each (`source_step` 64 KiB) or all on one page (0).
void map_gpu_pages(uint64_t address, const int64_t* pages, uint64_t count, uint64_t source, uint64_t source_step);
// Where the CPU reads the bytes the GPU sees at `address`: for pages the GPU's own mapping
// placed, the pages it put there; elsewhere `address` itself. nullptr where the GPU sees
// nothing. `*bytes`: how many bytes from there stay in a row.
const void* gpu_bytes_at(uint64_t address, uint64_t* bytes);
// Where the title's CPU writes the bytes the GPU sees at [address, address + size), as
// [first, end) ranges: its views of the pages the GPU's own mapping placed, elsewhere the
// addresses themselves. False where the GPU sees pages no view of the title's shows.
bool gpu_write_ranges(uint64_t address, uint64_t size, std::vector<std::pair<uint64_t, uint64_t>>& out);

}  // namespace am::rt::d3d12x
