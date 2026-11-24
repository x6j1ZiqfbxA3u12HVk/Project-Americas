// Views and descriptor copies (device slots 17-25, which follow D3D12's order), and resource
// allocation sizes.
//
// On the console a descriptor is memory the title owns: its own code copies descriptors and the
// GPU reads them. Each view therefore writes a small record into its descriptor slot (which view,
// of which resource); copies move those records like any memory, so a copied descriptor keeps
// its meaning. The GPU back end will turn records into host descriptors.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstring>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/formats.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

constexpr uint32_t kRecordMagic = 0x56444D41;  // "AMDV"

enum class ViewKind : uint16_t { Null = 0, Cbv, Srv, Uav, Rtv, Dsv, Sampler };

// The first 16 bytes of every descriptor slot we write (sampler slots are only 16 bytes).
struct Record {
  uint32_t magic;
  ViewKind kind;
  uint16_t format;
  uint64_t resource;  // the resource object, or a GPU address for CBVs
};
static_assert(sizeof(Record) == 16);

void write_record(uint64_t dest, ViewKind kind, uint32_t format, uint64_t resource) {
  if (!dest) return;
  Record r{kRecordMagic, kind, static_cast<uint16_t>(format), resource};
  std::memcpy(reinterpret_cast<void*>(dest), &r, sizeof r);
}

void WINAPI create_cbv(Object* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, uint64_t dest) {
  trace(self, 17, "CreateConstantBufferView", reinterpret_cast<uint64_t>(desc), dest, 0);
  write_record(dest, ViewKind::Cbv, 0, desc ? desc->BufferLocation : 0);
}

void WINAPI create_srv(Object* self, Object* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, uint64_t dest) {
  trace(self, 18, "CreateShaderResourceView", reinterpret_cast<uint64_t>(resource), reinterpret_cast<uint64_t>(desc), dest);
  write_record(dest, resource ? ViewKind::Srv : ViewKind::Null, desc ? desc->Format : 0, reinterpret_cast<uint64_t>(resource));
}

void WINAPI create_uav(Object* self, Object* resource, Object* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                       uint64_t dest) {
  trace(self, 19, "CreateUnorderedAccessView", reinterpret_cast<uint64_t>(resource), reinterpret_cast<uint64_t>(counter), dest);
  write_record(dest, resource ? ViewKind::Uav : ViewKind::Null, desc ? desc->Format : 0, reinterpret_cast<uint64_t>(resource));
}

void WINAPI create_rtv(Object* self, Object* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, uint64_t dest) {
  trace(self, 20, "CreateRenderTargetView", reinterpret_cast<uint64_t>(resource), reinterpret_cast<uint64_t>(desc), dest);
  write_record(dest, resource ? ViewKind::Rtv : ViewKind::Null, desc ? desc->Format : 0, reinterpret_cast<uint64_t>(resource));
}

void WINAPI create_dsv(Object* self, Object* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, uint64_t dest) {
  trace(self, 21, "CreateDepthStencilView", reinterpret_cast<uint64_t>(resource), reinterpret_cast<uint64_t>(desc), dest);
  write_record(dest, resource ? ViewKind::Dsv : ViewKind::Null, desc ? desc->Format : 0, reinterpret_cast<uint64_t>(resource));
}

void WINAPI create_sampler(Object* self, const D3D12_SAMPLER_DESC* desc, uint64_t dest) {
  trace(self, 22, "CreateSampler", reinterpret_cast<uint64_t>(desc), dest, 0);
  write_record(dest, ViewKind::Sampler, desc ? static_cast<uint32_t>(desc->Filter) : 0, 0);
}

// Slot 23: CopyDescriptors(dest ranges, source ranges, heap type). A null size array means
// ranges of one descriptor.
void WINAPI copy_descriptors(Object*, UINT dest_count, const uint64_t* dest_starts, const UINT* dest_sizes, UINT src_count,
                             const uint64_t* src_starts, const UINT* src_sizes, UINT type) {
  const uint32_t size = descriptor_size(type);
  UINT di = 0, si = 0, d_off = 0, s_off = 0;
  while (di < dest_count && si < src_count) {
    const UINT d_len = dest_sizes ? dest_sizes[di] : 1;
    const UINT s_len = src_sizes ? src_sizes[si] : 1;
    const UINT n = std::min(d_len - d_off, s_len - s_off);
    std::memmove(reinterpret_cast<void*>(dest_starts[di] + uint64_t{d_off} * size),
                 reinterpret_cast<const void*>(src_starts[si] + uint64_t{s_off} * size), uint64_t{n} * size);
    d_off += n;
    s_off += n;
    if (d_off == d_len) {
      ++di;
      d_off = 0;
    }
    if (s_off == s_len) {
      ++si;
      s_off = 0;
    }
  }
}

// Slot 24: CopyDescriptorsSimple(count, dest start, source start, heap type).
void WINAPI copy_descriptors_simple(Object*, UINT count, uint64_t dest, uint64_t src, UINT type) {
  if (!dest || !src || !count) return;
  std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), uint64_t{count} * descriptor_size(type));
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// The bytes and alignment one resource needs, by D3D12's rules: buffers in 64 KiB units; textures
// at 64 KiB (4 MiB when multisampled), or 4 KiB when the caller asks for it and the texture is
// small enough (64 KiB or less, not a render target or depth buffer).
D3D12_RESOURCE_ALLOCATION_INFO allocation_info(const D3D12_RESOURCE_DESC& d) {
  constexpr uint64_t k4K = 4096, k64K = 65536, k4M = 4u << 20;
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return {align_up(d.Width, k64K), k64K};
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN) return {UINT64_MAX, 0};
  FormatSize f = format_size(d.Format);
  if (!f.bytes) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) logf("d3d12x: no size for format %u; assuming 4 bytes per texel", d.Format);
    f = {4, 1};
  }
  const bool is_3d = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const uint32_t layers = is_3d ? 1 : std::max<uint32_t>(d.DepthOrArraySize, 1);
  uint32_t mips = d.MipLevels;
  if (mips == 0) {
    uint64_t largest = std::max<uint64_t>({d.Width, d.Height, is_3d ? d.DepthOrArraySize : 1u});
    while (largest) {
      ++mips;
      largest >>= 1;
    }
  }
  const uint32_t samples = std::max<uint32_t>(d.SampleDesc.Count, 1);
  uint64_t layer_bytes = 0;
  for (uint32_t m = 0; m < mips; ++m) {
    const uint64_t w = std::max<uint64_t>(d.Width >> m, 1), h = std::max<uint64_t>(d.Height >> m, 1);
    const uint64_t depth = is_3d ? std::max<uint64_t>(uint64_t{d.DepthOrArraySize} >> m, 1) : 1;
    const uint64_t bw = (w + f.block - 1) / f.block, bh = (h + f.block - 1) / f.block;
    layer_bytes += align_up(bw * f.bytes, 256) * bh * depth * samples;
  }
  const uint64_t total = layer_bytes * layers;
  const bool target = (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
  uint64_t alignment = samples > 1 ? k4M : k64K;
  if (d.Alignment == k4K && !target && samples == 1 && total <= k64K) alignment = k4K;
  return {align_up(total, alignment), alignment};
}

// Slot 25: GetResourceAllocationInfo(visible mask, count, descriptions): struct return.
D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_resource_allocation_info(uint64_t a0, uint64_t a1, UINT, UINT count,
                                                                    const D3D12_RESOURCE_DESC* descs) {
  D3D12_RESOURCE_ALLOCATION_INFO* out = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(is_object(reinterpret_cast<void*>(a1)) ? a0 : a1);
  uint64_t size = 0, alignment = 0;
  for (UINT i = 0; descs && i < count; ++i) {
    const D3D12_RESOURCE_ALLOCATION_INFO r = allocation_info(descs[i]);
    if (r.SizeInBytes == UINT64_MAX) {
      size = UINT64_MAX;
      break;
    }
    alignment = std::max<uint64_t>(alignment, r.Alignment);
    size = align_up(size, r.Alignment) + r.SizeInBytes;
  }
  *out = {size, alignment};
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 24 && descs && count) {
    logf("d3d12x: GetResourceAllocationInfo(dim %u, %llux%u x%u, %u mips, format %u, %u samples, flags 0x%x, alignment %llu) = %llu bytes, alignment %llu",
         descs[0].Dimension, static_cast<unsigned long long>(descs[0].Width), descs[0].Height, descs[0].DepthOrArraySize,
         descs[0].MipLevels, descs[0].Format, descs[0].SampleDesc.Count, descs[0].Flags,
         static_cast<unsigned long long>(descs[0].Alignment), static_cast<unsigned long long>(size),
         static_cast<unsigned long long>(alignment));
  }
  return out;
}

}  // namespace

void add_view_methods(std::vector<Method>& out) {
  out.insert(out.end(), {
      {17, "CreateConstantBufferView", reinterpret_cast<void*>(&create_cbv)},
      {18, "CreateShaderResourceView", reinterpret_cast<void*>(&create_srv)},
      {19, "CreateUnorderedAccessView", reinterpret_cast<void*>(&create_uav)},
      {20, "CreateRenderTargetView", reinterpret_cast<void*>(&create_rtv)},
      {21, "CreateDepthStencilView", reinterpret_cast<void*>(&create_dsv)},
      {22, "CreateSampler", reinterpret_cast<void*>(&create_sampler)},
      {23, "CopyDescriptors", reinterpret_cast<void*>(&copy_descriptors)},
      {24, "CopyDescriptorsSimple", reinterpret_cast<void*>(&copy_descriptors_simple)},
      {25, "GetResourceAllocationInfo", reinterpret_cast<void*>(&get_resource_allocation_info)},
  });
}

}  // namespace am::rt::d3d12x
