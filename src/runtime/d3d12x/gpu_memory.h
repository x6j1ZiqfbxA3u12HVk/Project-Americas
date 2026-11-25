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
//   shader heap [1 + n]  window n (raw, writable)
//
// Physical pages come from two pools (memory/title_memory.h). The committed pool (the game's
// small batches: descriptors, per-frame data) is one window too; the title memory reports every
// view of its pages and the page table follows. The pool committed on touch (the streaming
// heap) cannot be opened in place: its pages get VRAM copies instead (to come).
//
// Translated programs use one root signature: 32 root constants at b0 (the user data SGPRs),
// views from the shader heap by index.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

namespace am::rt::d3d12x {

constexpr uint32_t kPageTableIndex = 0;
constexpr uint32_t kUserDataConstants = 32;

// Opens the window of the title allocation holding `address` (if it is not open yet); false
// when the address is not in one we can open.
bool open_gpu_window(uint64_t address);
// The shader-visible heap and the root signature of translated programs; nullptr without a
// host GPU.
ID3D12DescriptorHeap* translated_shader_heap();
ID3D12RootSignature* translated_root_signature();
// Graphics: the vertex stage's user data at root parameter 0, the pixel stage's at 1 (both b0,
// 32 constants, each visible to its stage).
ID3D12RootSignature* translated_graphics_root_signature();
// Records the page table's pending updates into `list` (before work that reads them).
void flush_page_table(ID3D12GraphicsCommandList* list);

}  // namespace am::rt::d3d12x
