// xg_xs.dll, the console's texture layout library. The game imports two functions:
//
//   XGSuggestSwizzleMode(description, flags, &layout): fills a D3D12 description's Layout field
//     (0x100 plus a swizzle mode) for textures whose headers name no mode;
//   XGCreateTextureComputer(description, &computer): an object answering a texture's layout.
//
// Both describe textures by D3D12's resource description. The computer has no IUnknown: slot 0
// is AddRef and slot 1 Release. Slot 2 fills the texture's layout (size, planes, each mip's
// offset, size and pitch). The game calls slot 8 with (texture memory, plane, subresource,
// linear data, row pitch, slice pitch): the copy of linear data into the texture's layout. Its
// other slots are probes until the game's calls show them. Slot 10 is GetResourceTiling (D3D12's
// without the resource), which the texture streamer maps 64 KiB tiles by. Every answer comes
// from layout.h, the model GetResourceAllocationInfo uses: the two must agree (the streamer
// maps tiles by one and releases pages by the other's size).

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <set>

#include "runtime/d3d12x/bindings.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"
#include "runtime/process.h"

namespace am::rt::d3d12x {
namespace {

// Each place in the title that asks XG something is logged once (which code depends on our
// answers), with the first texture it asked about.
void note_caller(const char* what, const void* caller, const D3D12_RESOURCE_DESC& d, uint64_t size) {
  static std::mutex lock;
  static std::set<const void*> seen;
  {
    std::lock_guard g(lock);
    if (seen.size() > 64 || !seen.insert(caller).second) return;
  }
  logf("xg: %s asked from %s: %llux%u, %u mips, format %u: %llu bytes", what,
       process() ? process()->describe_address(reinterpret_cast<uint64_t>(caller)).c_str() : "?",
       static_cast<unsigned long long>(d.Width), d.Height, d.MipLevels, d.Format, static_cast<unsigned long long>(size));
}

struct TextureComputer : Object {
  D3D12_RESOURCE_DESC desc{};
  TextureLayout layout;
};

void log_desc(const char* what, const D3D12_RESOURCE_DESC& d, uint32_t mode) {
  logf("xg: %s(dim %u, %llux%u x%u, %u mips, format %u, %u samples, layout 0x%x, flags 0x%x, alignment %llu): mode %u",
       what, d.Dimension, static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, d.MipLevels, d.Format,
       d.SampleDesc.Count, static_cast<uint32_t>(d.Layout), d.Flags, static_cast<unsigned long long>(d.Alignment), mode);
}

// What slot 2 fills: the texture, then up to 4 planes of up to 15 mips each.
struct MipLayout {
  uint64_t size, offset, slice_size;
  uint32_t pitch_texels, pitch_bytes, alignment;
  uint32_t padded[3];      // width and height in elements, depth or array size
  uint32_t elements[2], texels[2];
  uint32_t depth_or_array, samples, swizzle_mode;
  uint64_t bank_rotation[2];
  uint32_t slice_depth, in_tail, tail_position;
};
struct PlaneLayout {
  uint32_t usage;  // 1 data, 7 depth, 8 stencil
  uint64_t size, slice_size, offset, alignment;
  uint32_t element_bytes, standard_mips, tail_mips;
  MipLayout mips[15];
};
struct ResourceLayout {
  uint64_t size, alignment;
  uint32_t mips, planes;
  PlaneLayout plane[4];
  uint32_t dimension;
};
static_assert(sizeof(MipLayout) == 0x70 && sizeof(PlaneLayout) == 0x6c8 && sizeof(ResourceLayout) == 0x1b40);

// Slot 2: GetResourceLayout(&layout). One plane (no separate stencil plane yet); every mip is
// reported as a standard one (our layouts keep no mip tail).
HRESULT WINAPI get_resource_layout(TextureComputer* self, ResourceLayout* out) {
  if (!out) return E_INVALIDARG;
  std::memset(out, 0, sizeof *out);
  const TextureLayout& l = self->layout;
  const D3D12_RESOURCE_DESC& d = self->desc;
  note_caller("GetResourceLayout", _ReturnAddress(), d, l.size);
  const bool three_d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const uint32_t mips = std::min<uint32_t>(l.mips, 15), array = three_d ? 1 : std::max<uint32_t>(d.DepthOrArraySize, 1);
  out->size = l.size;
  out->alignment = l.alignment;
  out->mips = l.mips;
  out->planes = 1;
  out->dimension = d.Dimension;
  PlaneLayout& p = out->plane[0];
  p.usage = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && l.mips == 1 ? 7 : 1;
  p.size = l.size;
  p.slice_size = l.size / array;
  p.alignment = l.alignment;
  p.element_bytes = l.element_bytes;
  p.standard_mips = std::min(l.first_tail_mip, mips);
  p.tail_mips = mips - p.standard_mips;
  for (uint32_t i = 0; i < mips && i < l.subresources.size(); ++i) {
    const SubresourceLayout& s = l.subresources[i];
    MipLayout& m = p.mips[i];
    const uint32_t pitch_elements = l.element_bytes ? s.row_pitch / l.element_bytes : 0;
    m.offset = s.offset;
    m.slice_size = uint64_t{s.row_pitch} * s.padded_rows * l.samples;
    m.size = m.slice_size * (three_d ? s.depth : 1);
    m.pitch_texels = pitch_elements * l.element_texels;
    m.pitch_bytes = s.row_pitch;
    m.alignment = static_cast<uint32_t>(std::min<uint64_t>(l.alignment, UINT32_MAX));
    m.padded[0] = pitch_elements;
    m.padded[1] = s.padded_rows;
    m.padded[2] = three_d ? s.depth : array;
    m.elements[0] = s.width_elements;
    m.elements[1] = s.rows;
    m.texels[0] = std::max<uint32_t>(static_cast<uint32_t>(d.Width >> i), 1);
    m.texels[1] = std::max<uint32_t>(d.Height >> i, 1);
    m.depth_or_array = three_d ? s.depth : array;
    m.samples = l.samples;
    m.swizzle_mode = l.swizzle_mode;
    m.slice_depth = 1;
    m.in_tail = s.in_tail ? 1 : 0;
    m.tail_position = s.in_tail ? i - l.first_tail_mip : 0;
  }
  return S_OK;
}

// Slot 8: CopyIntoSubresource(texture memory, plane, subresource, linear data, row pitch,
// slice pitch).
HRESULT WINAPI copy_into_subresource(TextureComputer* self, void* memory, UINT plane, UINT subresource,
                                     const void* linear, UINT row_pitch, UINT slice_pitch) {
  trace(self, 8, "CopyIntoSubresource", reinterpret_cast<uint64_t>(memory), subresource, reinterpret_cast<uint64_t>(linear));
  if (!memory || !linear || plane != 0 || subresource >= self->layout.subresources.size()) return E_INVALIDARG;
  const SubresourceLayout& s = self->layout.subresources[subresource];
  const uint64_t row_bytes = uint64_t{s.width_elements} * self->layout.element_bytes;
  const uint64_t slice_bytes = uint64_t{s.row_pitch} * s.padded_rows;
  auto* to = static_cast<uint8_t*>(memory) + s.offset;
  const auto* from = static_cast<const uint8_t*>(linear);
  if (s.in_tail) {
    // In the mip tail: row by row from (tail_x, tail_y) of the tail block, as the upload reads it.
    const uint32_t eb = self->layout.element_bytes, pitch = self->layout.block_width;
    for (uint32_t y = 0; y < s.rows; ++y) {
      const uint64_t at = (uint64_t{s.tail_y + y} * pitch + s.tail_x) * eb;
      if (at + row_bytes > self->layout.block_bytes) break;
      std::memcpy(to + at, from + uint64_t{y} * row_pitch, std::min<uint64_t>(row_bytes, row_pitch ? row_pitch : row_bytes));
    }
    notify_title_write(reinterpret_cast<uint64_t>(to), self->layout.block_bytes);
    return S_OK;
  }
  for (uint32_t z = 0; z < s.depth; ++z) {
    for (uint32_t y = 0; y < s.rows; ++y) {
      std::memcpy(to + z * slice_bytes + uint64_t{y} * s.row_pitch, from + uint64_t{z} * slice_pitch + uint64_t{y} * row_pitch,
                  std::min<uint64_t>(row_bytes, row_pitch ? row_pitch : row_bytes));
    }
  }
  notify_title_write(reinterpret_cast<uint64_t>(to), slice_bytes * s.depth);  // its texture is uploaded again
  return S_OK;
}

// Slot 10: GetResourceTiling(&tiles, &packed mips, &tile shape, &tiling count, first tiling,
// tilings), D3D12's without the resource (the UI texture streamer maps a texture's 64 KiB
// tiles from it). Over our layout: the standard 64 KiB tile shapes by element size; a mip is
// a standard one while it covers a whole tile in both directions, the rest are packed into
// the tiles from the first of them on. Each tile starts at its mip's offset in 64 KiB steps.
HRESULT WINAPI get_resource_tiling(TextureComputer* self, UINT* tiles, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape,
                                   UINT* count, UINT first, D3D12_SUBRESOURCE_TILING* tilings) {
  // AMERICAS_NO_XG_TILING=1 (diagnosis) leaves the game's zeros, as the first stub did.
  static const bool enabled = GetEnvironmentVariableA("AMERICAS_NO_XG_TILING", nullptr, 0) == 0;
  if (!enabled) return S_OK;
  constexpr uint64_t kTile = 64 << 10;
  const TextureLayout& l = self->layout;
  const D3D12_RESOURCE_DESC& d = self->desc;
  const bool three_d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const uint32_t array = three_d ? 1 : std::max<uint32_t>(d.DepthOrArraySize, 1);
  const uint32_t mips = std::min<uint32_t>(l.mips, static_cast<uint32_t>(l.subresources.size()));
  // 64 KiB of elements: 256x256 of 1 byte down to 64x64 of 16; in texels for block formats.
  uint32_t w = 64, h = 64;
  switch (l.element_bytes) {
    case 1: w = 256, h = 256; break;
    case 2: w = 256, h = 128; break;
    case 4: w = 128, h = 128; break;
    case 8: w = 128, h = 64; break;
    default: break;
  }
  const uint32_t block = std::max<uint32_t>(l.element_texels, 1);
  w *= block, h *= block;
  // The standard mips are the layout's (those before its mip tail); the tail is the packed
  // mips, in the chain's first tile.
  const uint32_t standard = std::min(l.first_tail_mip, mips);
  const uint64_t slice_bytes = l.size / array;
  const uint64_t packed_at = standard < mips ? l.subresources[standard].offset : slice_bytes;
  if (tiles) *tiles = static_cast<UINT>((l.size + kTile - 1) / kTile);
  if (packed) {
    packed->NumStandardMips = static_cast<UINT8>(standard);
    packed->NumPackedMips = static_cast<UINT8>(mips - standard);
    packed->NumTilesForPackedMips = standard < mips ? 1 : 0;
    packed->StartTileIndexInOverallResource = standard < mips ? static_cast<UINT>(packed_at / kTile) : 0;
  }
  if (shape) *shape = {w, h, 1};
  if (count) {
    const uint32_t subresources = mips * array, wanted = *count;
    uint32_t n = 0;
    for (uint32_t k = first; k < subresources && n < wanted && tilings; ++k, ++n) {
      const uint32_t mip = k % mips, slice = k / mips;
      D3D12_SUBRESOURCE_TILING& t = tilings[n];
      if (mip >= standard) {
        t = {0, 0, 0, D3D12_PACKED_TILE};
        continue;
      }
      t.WidthInTiles = (std::max<uint32_t>(static_cast<uint32_t>(d.Width >> mip), 1) + w - 1) / w;
      t.HeightInTiles = static_cast<UINT16>((std::max<uint32_t>(d.Height >> mip, 1) + h - 1) / h);
      t.DepthInTiles = 1;
      t.StartTileIndexInOverallResource = static_cast<UINT>((slice * slice_bytes + l.subresources[mip].offset) / kTile);
    }
    *count = n;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
    log_desc("GetResourceTiling", d, l.swizzle_mode);
    logf("xg: %u tiles of %ux%u, %u standard mips, packed from tile %llu", tiles ? *tiles : 0, w, h, standard,
         static_cast<unsigned long long>(packed_at / kTile));
  }
  return S_OK;
}

const Kind& computer_kind() {
  static const Kind k("XG texture computer", 32,
                      {
                          {0, "AddRef", reinterpret_cast<void*>(&add_ref)},
                          {1, "Release", reinterpret_cast<void*>(&release)},
                          {2, "GetResourceLayout", reinterpret_cast<void*>(&get_resource_layout)},
                          {8, "CopyIntoSubresource", reinterpret_cast<void*>(&copy_into_subresource)},
                          {10, "GetResourceTiling", reinterpret_cast<void*>(&get_resource_tiling)},
                      });
  return k;
}

}  // namespace

HRESULT WINAPI xg_suggest_swizzle_mode(const D3D12_RESOURCE_DESC* desc, UINT /*flags*/, UINT* layout) {
  if (!desc || !layout) return E_INVALIDARG;
  const uint32_t mode = suggest_swizzle_mode(*desc);
  *layout = kLayoutSwizzleFlag | mode;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) log_desc("XGSuggestSwizzleMode", *desc, mode);
  return S_OK;
}

HRESULT WINAPI xg_create_texture_computer(const D3D12_RESOURCE_DESC* desc, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!desc) return E_INVALIDARG;
  TextureLayout layout;
  if (!texture_layout(*desc, layout)) {
    log_desc("XGCreateTextureComputer refused", *desc, 0);
    return E_INVALIDARG;
  }
  auto* c = make_object<TextureComputer>(computer_kind());
  c->desc = *desc;
  c->layout = std::move(layout);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) log_desc("XGCreateTextureComputer", *desc, c->layout.swizzle_mode);
  *out = c;
  return S_OK;
}

}  // namespace am::rt::d3d12x
