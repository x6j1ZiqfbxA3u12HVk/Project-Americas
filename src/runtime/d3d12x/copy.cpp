// The command lists' copies (CopyBufferRegion, CopyTextureRegion, CopyResource and the
// console's CopyTextureRegionX / CopyResourceX), done in title memory when the list is
// submitted, in list order: on the console a copy writes the destination's memory, which the
// title's later work and our texture uploads read. Textures keep their elements linear in each
// subresource (layout.h), so a region copy is a copy of rows; mip-tail subresources sit at their
// slot in the tail block. Copies from memory the GPU writes (render targets, storage textures)
// cannot be done here and are skipped, counted and logged.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "runtime/d3d12x/bindings.h"
#include "runtime/d3d12x/commands.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/host.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

std::atomic<uint64_t> g_copies_done{0}, g_copies_skipped{0};

bool move_bytes(uint64_t to, uint64_t from, uint64_t size) {
  __try {
    std::memmove(reinterpret_cast<void*>(to), reinterpret_cast<const void*>(from), static_cast<size_t>(size));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void skipped(const char* why) {
  ++g_copies_skipped;
  static std::atomic<int> told{0};
  if (told.fetch_add(1, std::memory_order_relaxed) < 16) logf("gpu: a copy not done: %s", why);
}

// One side of a region copy: where element (x, y) of depth slice z is.
struct Surface {
  uint64_t base = 0;            // the subresource's (or footprint's) first byte
  uint64_t row_pitch = 0;       // bytes
  uint64_t slice_pitch = 0;     // bytes
  uint32_t width = 0, rows = 0, depth = 1;  // in elements
  uint64_t notify_bytes = 0;    // what a write to it changes, from base
  uint64_t at(uint32_t x, uint32_t y, uint32_t z, uint32_t element_bytes) const {
    return base + z * slice_pitch + y * row_pitch + uint64_t{x} * element_bytes;
  }
};

// A texture subresource of `resource`.
bool texture_surface(const void* resource, uint32_t subresource, Surface& s, TextureLayout& l) {
  D3D12_RESOURCE_DESC d{};
  uint64_t address = 0;
  if (!resource_memory(resource, &d, &address) || !texture_layout(d, l) || subresource >= l.subresources.size()) return false;
  const SubresourceLayout& sub = l.subresources[subresource];
  s.width = sub.width_elements;
  s.rows = sub.rows;
  s.depth = sub.depth;
  if (sub.in_tail) {
    // Rows of the tail block, from (tail_x, tail_y).
    s.row_pitch = uint64_t{l.block_width} * l.element_bytes;
    s.slice_pitch = l.block_bytes;
    s.base = address + sub.offset + (uint64_t{sub.tail_y} * l.block_width + sub.tail_x) * l.element_bytes;
    s.notify_bytes = l.block_bytes;
  } else {
    s.row_pitch = sub.row_pitch;
    s.slice_pitch = uint64_t{sub.row_pitch} * sub.padded_rows;
    s.base = address + sub.offset;
    s.notify_bytes = s.slice_pitch * sub.depth;
  }
  return true;
}

// A placed footprint in a buffer, in elements of `element_bytes` covering `texels` texels.
bool footprint_surface(const D3D12_TEXTURE_COPY_LOCATION& loc, uint32_t element_bytes, uint32_t texels, Surface& s) {
  D3D12_RESOURCE_DESC d{};
  uint64_t address = 0;
  if (!resource_memory(loc.pResource, &d, &address)) return false;
  const D3D12_SUBRESOURCE_FOOTPRINT& f = loc.PlacedFootprint.Footprint;
  s.base = address + loc.PlacedFootprint.Offset;
  s.row_pitch = f.RowPitch;
  s.width = (f.Width + texels - 1) / texels;
  s.rows = (f.Height + texels - 1) / texels;
  s.depth = std::max<uint32_t>(f.Depth, 1);
  s.slice_pitch = uint64_t{f.RowPitch} * s.rows;
  s.notify_bytes = s.slice_pitch * s.depth;
  (void)element_bytes;
  return true;
}

bool gpu_written(uint64_t address) {
  D3D12_RESOURCE_DESC d{};
  return host_texture_at(address, &d) != nullptr;
}

void copy_buffer_region(const Command& c) {
  D3D12_RESOURCE_DESC dd{}, sd{};
  uint64_t da = 0, sa = 0;
  if (!resource_memory(reinterpret_cast<const void*>(c.a[0]), &dd, &da) || !resource_memory(reinterpret_cast<const void*>(c.a[2]), &sd, &sa)) {
    return skipped("CopyBufferRegion of an object that is not a resource of ours");
  }
  if (!c.a[4]) return;
  if (!move_bytes(da + c.a[1], sa + c.a[3], c.a[4])) return skipped("CopyBufferRegion outside readable memory");
  notify_title_write(da + c.a[1], c.a[4]);
  ++g_copies_done;
}

void copy_resource(const Command& c) {
  const auto* dst = reinterpret_cast<const void*>(c.a[0]);
  const auto* src = reinterpret_cast<const void*>(c.a[1]);
  D3D12_RESOURCE_DESC dd{}, sd{};
  uint64_t da = 0, sa = 0;
  if (!resource_memory(dst, &dd, &da) || !resource_memory(src, &sd, &sa)) return skipped("CopyResource of an object that is not a resource of ours");
  uint64_t bytes = 0;
  if (dd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    bytes = std::min<uint64_t>(dd.Width, sd.Width);
  } else {
    TextureLayout ld, ls;
    if (!texture_layout(dd, ld) || !texture_layout(sd, ls)) return skipped("CopyResource of textures we cannot lay out");
    if (ld.size != ls.size || ld.element_bytes != ls.element_bytes) return skipped("CopyResource between different layouts");
    bytes = ld.size;
  }
  if (gpu_written(sa)) return skipped("CopyResource from memory the GPU writes");
  if (!move_bytes(da, sa, bytes)) return skipped("CopyResource outside readable memory");
  notify_title_write(da, bytes);
  ++g_copies_done;
}

void copy_texture_region(const Command& c, const Recording& r) {
  // The kept data: the destination location, the source location, then the box when given.
  constexpr size_t kLoc = sizeof(D3D12_TEXTURE_COPY_LOCATION);
  if (c.data_size < 2 * kLoc || c.data_at + c.data_size > r.data.size()) return skipped("CopyTextureRegion without its locations");
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  std::memcpy(&dst, r.data.data() + c.data_at, kLoc);
  std::memcpy(&src, r.data.data() + c.data_at + kLoc, kLoc);
  D3D12_BOX box{};
  const bool has_box = c.a[5] != 0 && c.data_size >= 2 * kLoc + sizeof box;
  if (has_box) std::memcpy(&box, r.data.data() + c.data_at + 2 * kLoc, sizeof box);
  // Element size from whichever side is a texture.
  TextureLayout ld, ls;
  Surface sd, ss;
  const bool dst_texture = dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const bool src_texture = src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  if (dst_texture && !texture_surface(dst.pResource, dst.SubresourceIndex, sd, ld)) return skipped("CopyTextureRegion to a texture we cannot lay out");
  if (src_texture && !texture_surface(src.pResource, src.SubresourceIndex, ss, ls)) return skipped("CopyTextureRegion from a texture we cannot lay out");
  if (!dst_texture && !src_texture) return skipped("CopyTextureRegion between two footprints");
  const TextureLayout& l = dst_texture ? ld : ls;
  const uint32_t eb = l.element_bytes, et = std::max<uint32_t>(l.element_texels, 1);
  if (dst_texture && src_texture && ls.element_bytes != ld.element_bytes) return skipped("CopyTextureRegion between element sizes");
  if (!dst_texture && !footprint_surface(dst, eb, et, sd)) return skipped("CopyTextureRegion to a footprint of no resource");
  if (!src_texture && !footprint_surface(src, eb, et, ss)) return skipped("CopyTextureRegion from a footprint of no resource");
  // The source region and the destination's corner, in elements.
  uint32_t x0 = 0, y0 = 0, z0 = 0, w = ss.width, h = ss.rows, d = ss.depth;
  if (has_box) {
    x0 = box.left / et;
    y0 = box.top / et;
    z0 = box.front;
    w = (box.right - box.left + et - 1) / et;
    h = (box.bottom - box.top + et - 1) / et;
    d = std::max<uint32_t>(box.back - box.front, 1);
  }
  const uint32_t dx = static_cast<uint32_t>(c.a[1]) / et, dy = static_cast<uint32_t>(c.a[2]) / et, dz = static_cast<uint32_t>(c.a[3]);
  w = std::min({w, ss.width > x0 ? ss.width - x0 : 0u, sd.width > dx ? sd.width - dx : 0u});
  h = std::min({h, ss.rows > y0 ? ss.rows - y0 : 0u, sd.rows > dy ? sd.rows - dy : 0u});
  d = std::min({d, ss.depth > z0 ? ss.depth - z0 : 0u, sd.depth > dz ? sd.depth - dz : 0u});
  if (!w || !h || !d) return;
  if (src_texture && gpu_written(ss.base)) return skipped("CopyTextureRegion from memory the GPU writes");
  for (uint32_t z = 0; z < d; ++z) {
    for (uint32_t y = 0; y < h; ++y) {
      if (!move_bytes(sd.at(dx, dy + y, dz + z, eb), ss.at(x0, y0 + y, z0 + z, eb), uint64_t{w} * eb)) {
        return skipped("CopyTextureRegion outside readable memory");
      }
    }
  }
  notify_title_write(sd.base, sd.notify_bytes);
  ++g_copies_done;
}

}  // namespace

void run_copies(const Recording& r) {
  for (const Command& c : r.commands) {
    switch (c.op) {
      case kCopyBufferRegion: copy_buffer_region(c); break;
      case kCopyResource: copy_resource(c); break;
      case kCopyTextureRegion: copy_texture_region(c, r); break;
      default: break;
    }
  }
}

uint64_t copies_done() { return g_copies_done.load(); }
uint64_t copies_skipped() { return g_copies_skipped.load(); }

}  // namespace am::rt::d3d12x
