#pragma once

// Texture memory layouts: one model used wherever a texture's size or placement is asked for
// (GetResourceAllocationInfo, committed resources, the console's XG library), so they agree.
//
// The console's D3D12 description carries a swizzle mode in its Layout field, as 0x100 plus the
// mode, numbered like the GPU's own (RDNA): 0 linear; 1-3 256-byte blocks; 4-7 and 20-23 4 KiB
// blocks; 8-11, 16-19 and 24-27 64 KiB blocks. The game's textures name their mode in their
// headers; for the others XGSuggestSwizzleMode picks one.
//
// Our model of a mode: each mip level is a grid of whole blocks, a block holding a power of two
// of elements (texels, or 4x4 blocks of a compressed format) shaped as on the GPU: 2^ceil(n/2)
// wide and 2^floor(n/2) high. Array layers hold their mip chains one after another, each layer
// a whole number of blocks. There is no mip tail packing, so a texture never takes less memory
// than on the console. Inside the padded grid, elements are stored row by row (the padded
// width is the pitch): the GPU's own order inside blocks is not reproduced yet.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace am::rt::d3d12x {

constexpr uint32_t kLayoutSwizzleFlag = 0x100;

struct SubresourceLayout {
  uint64_t offset = 0;          // from the texture's start
  uint64_t size = 0;            // bytes, depth slices included
  uint32_t row_pitch = 0;       // bytes from one row of elements to the next
  uint32_t rows = 0;            // rows of elements with data
  uint32_t padded_rows = 0;     // rows per depth slice in memory
  uint32_t width_elements = 0;  // elements with data in a row
  uint32_t depth = 1;           // depth slices (3D textures)
};

struct TextureLayout {
  uint32_t swizzle_mode = 0;
  uint32_t block_bytes = 256;
  uint32_t element_bytes = 4;
  uint32_t element_texels = 1;  // texels per element side: 4 for block-compressed formats
  uint32_t block_width = 1, block_height = 1;  // in elements
  uint32_t mips = 1, layers = 1, samples = 1;
  uint64_t size = 0;
  uint64_t alignment = 0;
  std::vector<SubresourceLayout> subresources;  // D3D12's order: mip + mips * layer
};

// The block size (bytes) a swizzle mode uses.
uint32_t swizzle_block_bytes(uint32_t mode);
// The mode a description asks for, or our choice when it names none.
uint32_t swizzle_mode_of(const D3D12_RESOURCE_DESC& d);
// Our choice for a texture: by usage (depth, render target or storage, sampled), with 4 KiB
// blocks for textures too small to fill a 64 KiB one.
uint32_t suggest_swizzle_mode(const D3D12_RESOURCE_DESC& d);
// False for buffers and descriptions we cannot size.
bool texture_layout(const D3D12_RESOURCE_DESC& d, TextureLayout& out);

}  // namespace am::rt::d3d12x
