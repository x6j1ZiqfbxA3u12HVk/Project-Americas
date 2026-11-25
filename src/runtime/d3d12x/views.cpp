// Views and descriptor copies (device slots 17-25, which follow D3D12's order), and resource
// allocation sizes (slots 25 and 108).
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
#include "runtime/d3d12x/layout.h"
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

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

void write_record(uint64_t dest, ViewKind kind, uint32_t format, uint64_t resource) {
  if (!dest) return;
  Record r{kRecordMagic, kind, static_cast<uint16_t>(format), resource};
  std::memcpy(reinterpret_cast<void*>(dest), &r, sizeof r);
}

// A CBV is a buffer descriptor (V#) over its range: translated shaders read it with
// s_buffer_load as the console GPU does.
void WINAPI create_cbv(Object* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, uint64_t dest) {
  trace(self, 17, "CreateConstantBufferView", reinterpret_cast<uint64_t>(desc), dest, 0);
  if (!dest) return;
  uint32_t v[8] = {};
  if (desc) encode_buffer_descriptor(v, desc->BufferLocation, desc->SizeInBytes, 0);
  std::memcpy(reinterpret_cast<void*>(dest), v, sizeof v);
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

// Slot 108: GetResourceAllocationInfo2(visible mask, count, D3D12_RESOURCE_DESC1 array,
// D3D12_RESOURCE_ALLOCATION_INFO1 array or null): struct return. The game asks with 4 KiB
// alignment first and again without when the answer's alignment differs.
D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_resource_allocation_info2(uint64_t a0, uint64_t a1, UINT, UINT count,
                                                                     const D3D12_RESOURCE_DESC1* descs,
                                                                     D3D12_RESOURCE_ALLOCATION_INFO1* each) {
  D3D12_RESOURCE_ALLOCATION_INFO* out = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(is_object(reinterpret_cast<void*>(a1)) ? a0 : a1);
  uint64_t size = 0, alignment = 0;
  for (UINT i = 0; descs && i < count; ++i) {
    D3D12_RESOURCE_DESC d{};
    std::memcpy(&d, &descs[i], sizeof d);  // DESC1 begins with DESC's fields
    const D3D12_RESOURCE_ALLOCATION_INFO r = allocation_info(d);
    if (r.SizeInBytes == UINT64_MAX) {
      size = UINT64_MAX;
      if (each) each[i] = {UINT64_MAX, 0, UINT64_MAX};
      break;
    }
    alignment = std::max<uint64_t>(alignment, r.Alignment);
    const uint64_t offset = align_up(size, r.Alignment);
    if (each) each[i] = {offset, r.Alignment, r.SizeInBytes};
    size = offset + r.SizeInBytes;
  }
  *out = {size, alignment};
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 24 && descs && count) {
    logf("d3d12x: GetResourceAllocationInfo2(dim %u, %llux%u x%u, %u mips, format %u, layout 0x%x, flags 0x%x, alignment %llu) = %llu bytes, alignment %llu",
         descs[0].Dimension, static_cast<unsigned long long>(descs[0].Width), descs[0].Height, descs[0].DepthOrArraySize,
         descs[0].MipLevels, descs[0].Format, static_cast<uint32_t>(descs[0].Layout), descs[0].Flags,
         static_cast<unsigned long long>(descs[0].Alignment), static_cast<unsigned long long>(size),
         static_cast<unsigned long long>(alignment));
  }
  return out;
}

}  // namespace

// RDNA 2's buffer descriptor: base address (48 bits), stride (14 bits), number of records, then
// the destination channel selects (x y z w), a 32-bit float format, resource level 1, and an
// out-of-bounds mode of 3 (raw: bounds by bytes) when the stride is 0, else 0 (by index).
void encode_buffer_descriptor(uint32_t out[4], uint64_t base, uint64_t bytes, uint32_t stride) {
  constexpr uint32_t kFormat32Float = 22;  // BUF_FMT_32_FLOAT
  out[0] = static_cast<uint32_t>(base);
  out[1] = static_cast<uint32_t>(base >> 32) & 0xFFFFu;
  out[1] |= (stride & 0x3FFFu) << 16;
  out[2] = static_cast<uint32_t>(stride ? bytes / stride : std::min<uint64_t>(bytes, 0xFFFFFFFFu));
  out[3] = 4u | (5u << 3) | (6u << 6) | (7u << 9) | (kFormat32Float << 12) | (1u << 24) | ((stride ? 0u : 3u) << 28);
}

void write_shader_view(uint64_t dest, const D3D12_RESOURCE_DESC* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view,
                       uint64_t address) {
  if (!dest) return;
  if (view.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
    const uint32_t stride = view.Buffer.StructureByteStride;
    const bool raw = (view.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    const FormatSize f = format_size(view.Format);
    const uint64_t element = raw ? 4 : stride ? stride : std::max<uint32_t>(f.bytes, 1);
    uint32_t v[8] = {};
    encode_buffer_descriptor(v, address + view.Buffer.FirstElement * element, view.Buffer.NumElements * element,
                             raw ? 0u : static_cast<uint32_t>(element));
    std::memcpy(reinterpret_cast<void*>(dest), v, sizeof v);
    return;
  }
  write_record(dest, ViewKind::Srv, view.Format, address);
  (void)resource;
}

bool read_view(uint64_t descriptor, ViewInfo& out) {
  if (!descriptor) return false;
  Record r{};
  __try {
    std::memcpy(&r, reinterpret_cast<const void*>(descriptor), sizeof r);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (r.magic != kRecordMagic) return false;
  out.kind = static_cast<uint16_t>(r.kind);
  out.format = r.format;
  out.resource = r.resource;
  return true;
}

// The bytes and alignment one resource needs: buffers in 64 KiB units, textures by our layout
// model (layout.h), which the console's XG library answers with too.
D3D12_RESOURCE_ALLOCATION_INFO allocation_info(const D3D12_RESOURCE_DESC& d) {
  constexpr uint64_t k64K = 65536;
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return {align_up(d.Width, k64K), k64K};
  TextureLayout layout;
  if (!texture_layout(d, layout)) return {UINT64_MAX, 0};
  return {layout.size, layout.alignment};
}

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
      {108, "GetResourceAllocationInfo2", reinterpret_cast<void*>(&get_resource_allocation_info2)},
  });
}

}  // namespace am::rt::d3d12x
