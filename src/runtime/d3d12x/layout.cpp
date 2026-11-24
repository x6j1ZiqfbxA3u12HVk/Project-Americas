#include "runtime/d3d12x/layout.h"

#include <algorithm>
#include <atomic>
#include <bit>

#include "runtime/d3d12x/formats.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

constexpr uint64_t k4K = 4096, k64K = 65536;

// RDNA's swizzle modes by usage: Z for depth, R for render targets and storage, S for sampled
// textures; the _X variants (4 KiB: 20-23, 64 KiB: 24-27) are the GPU's usual choice.
constexpr uint32_t kS4K = 21, kZ64K = 24, kS64K = 25, kR64K = 27;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

FormatSize element_of(uint32_t format) {
  FormatSize f = format_size(format);
  if (!f.bytes) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      logf("d3d12x: no size for format %u; assuming 4 bytes per texel", format);
    }
    f = {4, 1};
  }
  return f;
}

uint32_t mip_count(const D3D12_RESOURCE_DESC& d) {
  if (d.MipLevels) return d.MipLevels;
  const bool is_3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  uint64_t largest = std::max<uint64_t>({d.Width, d.Height, is_3d ? d.DepthOrArraySize : 1u});
  uint32_t n = 0;
  while (largest) {
    ++n;
    largest >>= 1;
  }
  return std::max(n, 1u);
}

}  // namespace

uint32_t swizzle_block_bytes(uint32_t mode) {
  if (mode <= 3) return 256;  // linear (256-byte pitch alignment) and the 256-byte modes
  if (mode <= 7 || (mode >= 20 && mode <= 23)) return 4096;
  return 65536;
}

uint32_t suggest_swizzle_mode(const D3D12_RESOURCE_DESC& d) {
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) return 0;
  const bool depth = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
  const bool target =
      (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0;
  if (depth) return kZ64K;
  if (target) return kR64K;
  // Small sampled textures get 4 KiB blocks: when the description asks for 4 KiB alignment
  // (D3D12's small-resource rule) or the first mip is smaller than one 64 KiB block. Render
  // targets and depth always get 64 KiB blocks, as in D3D12.
  const FormatSize f = element_of(d.Format);
  const uint64_t w = (d.Width + f.block - 1) / f.block, h = (uint64_t{d.Height} + f.block - 1) / f.block;
  const bool small_texture = d.Alignment == k4K || w * h * f.bytes < k64K;
  const bool single_sample = d.SampleDesc.Count <= 1;
  return small_texture && single_sample ? kS4K : kS64K;
}

uint32_t swizzle_mode_of(const D3D12_RESOURCE_DESC& d) {
  const uint32_t layout = static_cast<uint32_t>(d.Layout);
  if ((layout & ~0xFFu) == kLayoutSwizzleFlag && (layout & 0xFF) <= 31) return layout & 0xFF;
  return suggest_swizzle_mode(d);
}

bool texture_layout(const D3D12_RESOURCE_DESC& d, TextureLayout& out) {
  if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D && d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    return false;
  }
  if (!d.Width || !d.Height) return false;
  out = {};
  const FormatSize f = element_of(d.Format);
  out.element_bytes = f.bytes;
  out.element_texels = f.block;
  out.swizzle_mode = swizzle_mode_of(d);
  // Elements of a size that is not a power of two (96-bit formats) only exist linear.
  if (!std::has_single_bit(f.bytes)) out.swizzle_mode = 0;
  const bool linear = out.swizzle_mode == 0;
  out.block_bytes = swizzle_block_bytes(out.swizzle_mode);
  if (!linear) {
    const uint32_t n = std::countr_zero(out.block_bytes) - std::countr_zero(f.bytes);
    out.block_width = 1u << ((n + 1) / 2);
    out.block_height = 1u << (n / 2);
  }
  const bool is_3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  out.mips = mip_count(d);
  out.layers = is_3d ? 1 : std::max<uint32_t>(d.DepthOrArraySize, 1);
  out.samples = std::max<uint32_t>(d.SampleDesc.Count, 1);

  // One layer's mip chain.
  std::vector<SubresourceLayout> chain(out.mips);
  uint64_t at = 0;
  for (uint32_t m = 0; m < out.mips; ++m) {
    const uint64_t w = std::max<uint64_t>(d.Width >> m, 1), h = std::max<uint64_t>(d.Height >> m, 1);
    SubresourceLayout& s = chain[m];
    s.width_elements = static_cast<uint32_t>((w + f.block - 1) / f.block);
    s.rows = static_cast<uint32_t>((h + f.block - 1) / f.block);
    s.depth = is_3d ? std::max<uint32_t>(d.DepthOrArraySize >> m, 1) : 1;
    if (linear) {
      s.row_pitch = static_cast<uint32_t>(align_up(uint64_t{s.width_elements} * f.bytes, 256));
      s.padded_rows = s.rows;
      at = align_up(at, 256);
    } else {
      s.row_pitch = static_cast<uint32_t>(align_up(s.width_elements, out.block_width) * f.bytes);
      s.padded_rows = static_cast<uint32_t>(align_up(s.rows, out.block_height));
    }
    s.offset = at;
    s.size = uint64_t{s.row_pitch} * s.padded_rows * s.depth * out.samples;
    at += s.size;
  }
  const uint64_t layer_stride = align_up(at, out.block_bytes);
  out.subresources.reserve(uint64_t{out.mips} * out.layers);
  for (uint32_t l = 0; l < out.layers; ++l) {
    for (const SubresourceLayout& s : chain) {
      SubresourceLayout t = s;
      t.offset += layer_stride * l;
      out.subresources.push_back(t);
    }
  }
  out.alignment = out.block_bytes >= k64K || out.samples > 1 ? k64K : k4K;
  out.size = align_up(layer_stride * out.layers, out.alignment);
  return true;
}

}  // namespace am::rt::d3d12x
