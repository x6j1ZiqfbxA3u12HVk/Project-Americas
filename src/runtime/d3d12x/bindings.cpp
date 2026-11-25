#include "runtime/d3d12x/bindings.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/text.h"
#include "runtime/d3d12x/gpu_memory.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/log.h"
#include "runtime/present/host_gpu.h"
#include "shader/image_bindings.h"

namespace am::rt::d3d12x {
namespace {

using Microsoft::WRL::ComPtr;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// The title's memory may be bad: a fault leaves zeros.
bool read_guarded(const void* from, void* to, size_t size) {
  __try {
    std::memcpy(to, from, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    std::memset(to, 0, size);
    return false;
  }
}

struct Texture {
  uint64_t address = 0, size = 0;
  D3D12_RESOURCE_DESC desc{};
  TextureLayout layout;
  ComPtr<ID3D12Resource> resource;
  bool dirty = true;
};

// RDNA 2's sampler descriptor to D3D12's.
D3D12_SAMPLER_DESC sampler_from(const uint32_t w[4], bool& compare) {
  static const D3D12_TEXTURE_ADDRESS_MODE kModes[8] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP,   D3D12_TEXTURE_ADDRESS_MODE_MIRROR,      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};
  D3D12_SAMPLER_DESC d{};
  d.AddressU = kModes[w[0] & 7];
  d.AddressV = kModes[(w[0] >> 3) & 7];
  d.AddressW = kModes[(w[0] >> 6) & 7];
  const uint32_t aniso = (w[0] >> 9) & 7, func = (w[0] >> 12) & 7;
  compare = func != 0;
  const uint32_t mag = (w[2] >> 20) & 3, min = (w[2] >> 22) & 3, mip = (w[2] >> 26) & 3;
  const bool anisotropic = mag >= 2 || min >= 2;
  if (anisotropic && aniso) {
    d.Filter = compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << std::min<uint32_t>(aniso, 4);
  } else {
    const bool lin_mag = (mag & 1) != 0, lin_min = (min & 1) != 0, lin_mip = mip == 2;
    const int f = (lin_min ? 0x10 : 0) | (lin_mag ? 0x4 : 0) | (lin_mip ? 0x1 : 0);
    d.Filter = static_cast<D3D12_FILTER>(f | (compare ? 0x80 : 0));
    d.MaxAnisotropy = 1;
  }
  d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(func + 1);  // the same order, D3D12's from 1
  d.MinLOD = static_cast<float>(w[1] & 0xFFF) / 256.0f;
  d.MaxLOD = static_cast<float>((w[1] >> 12) & 0xFFF) / 256.0f;
  if (mip == 0) d.MaxLOD = d.MinLOD;  // no mipmapping: the base level
  const int32_t bias = static_cast<int32_t>((w[2] & 0x3FFF) << 18) >> 18;  // s6.8 in 14 bits
  d.MipLODBias = static_cast<float>(bias) / 256.0f;
  const uint32_t border = w[3] >> 30;
  const float black_opaque[4] = {0, 0, 0, 1}, black[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 1};
  std::memcpy(d.BorderColor, border == 0 ? black_opaque : border == 2 ? white : black, sizeof d.BorderColor);
  return d;
}

struct Bindings {
  std::mutex lock;
  bool started = false, usable = false;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12DescriptorHeap> samplers;
  UINT sampler_step = 0, next_sampler = 0;
  std::vector<shader::ImageBinding> images;
  std::vector<shader::SamplerBinding> sampler_bindings;
  std::vector<Texture> textures;
  bool table_dirty = true;
  ComPtr<ID3D12Resource> table, upload, readback, header_upload;
  uint64_t table_bytes = 0;
  uint32_t misses_at = 0, misses_capacity = 0;
  // The misses' readback: recorded into a list, then submitted (with the fence value after it).
  enum class Readback { idle, recorded, in_flight } readback_state = Readback::idle;
  uint64_t readback_fence = 0;
  std::vector<ComPtr<ID3D12Resource>> retired;  // uploads of this submission, kept one more round

  bool start() {
    started = true;
    present::HostGpu* gpu = present::host_gpu();
    if (!gpu) return false;
    device = gpu->device;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&samplers)))) return false;
    sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    usable = true;
    return true;
  }

  ComPtr<ID3D12Resource> buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
  }

  void add_sampler(const uint32_t w[4]) {
    for (const auto& s : sampler_bindings) {
      if (std::equal(s.words, s.words + 4, w)) return;
    }
    if (next_sampler >= 2048) return;
    bool compare = false;
    const D3D12_SAMPLER_DESC d = sampler_from(w, compare);
    D3D12_CPU_DESCRIPTOR_HANDLE h = samplers->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T{next_sampler} * sampler_step;
    device->CreateSampler(&d, h);
    shader::SamplerBinding b;
    std::copy(w, w + 4, b.words);
    b.index = next_sampler++;
    b.flags = compare ? shader::kSamplerCompare : 0u;
    sampler_bindings.push_back(b);
    table_dirty = true;
  }

  // Reads the misses the GPU queued last time: samplers are added; unknown images are counted.
  void take_misses() {
    if (readback_state != Readback::in_flight || !present::host_gpu()->done(readback_fence)) return;
    readback_state = Readback::idle;
    const uint8_t* p = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&p))))) return;
    uint32_t written = 0;
    std::memcpy(&written, p + 24, 4);
    written = std::min(written, misses_capacity);
    static std::atomic<int> unknown_images{0};
    for (uint32_t k = 0; k < written; ++k) {
      uint32_t r[12];
      std::memcpy(r, p + 32 + size_t{k} * 48, sizeof r);
      if (r[0] == 2) add_sampler(r + 4);
      else if (r[0] == 1 && unknown_images.fetch_add(1) < 8) {
        logf("gpu: a shader used a texture we did not make (T# %08x %08x %08x %08x %08x %08x %08x %08x)", r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11]);
      }
    }
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
  }

  void upload_texture(ID3D12GraphicsCommandList* list, Texture& t) {
    t.dirty = false;
    const UINT subs = static_cast<UINT>(t.layout.subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> row_bytes(subs);
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC hd = t.resource->GetDesc();
    device->GetCopyableFootprints(&hd, 0, subs, 0, fp.data(), rows.data(), row_bytes.data(), &total);
    ComPtr<ID3D12Resource> up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    uint8_t* p = nullptr;
    if (!up || FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
    for (UINT k = 0; k < subs; ++k) {
      const SubresourceLayout& s = t.layout.subresources[k];
      const uint64_t bytes = std::min<uint64_t>(row_bytes[k], s.row_pitch);
      for (UINT z = 0; z < fp[k].Footprint.Depth; ++z) {
        for (UINT y = 0; y < rows[k] && y < s.rows; ++y) {
          const uint64_t from = t.address + s.offset + (uint64_t{z} * s.padded_rows + y) * s.row_pitch;
          read_guarded(reinterpret_cast<const void*>(from),
                       p + fp[k].Offset + (uint64_t{z} * rows[k] + y) * fp[k].Footprint.RowPitch, bytes);
        }
      }
    }
    up->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {t.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, &b);
    for (UINT k = 0; k < subs; ++k) {
      D3D12_TEXTURE_COPY_LOCATION dst{t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.SubresourceIndex = k;
      D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.PlacedFootprint = fp[k];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    retired.push_back(std::move(up));
  }

  void flush(ID3D12GraphicsCommandList* list) {
    if (!usable) return;
    // Uploads of two submissions ago have run (one queue, in order, a frame or more apart).
    if (retired.size() > 256) retired.erase(retired.begin(), retired.begin() + static_cast<ptrdiff_t>(retired.size() / 2));
    take_misses();
    for (Texture& t : textures) {
      if (t.dirty && t.resource) upload_texture(list, t);
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    if (table_dirty) {
      std::vector<uint32_t> words = shader::build_bindings(images, sampler_bindings, 256);
      if (words.empty()) return;
      const uint64_t bytes = words.size() * 4;
      if (!table || table_bytes < bytes) {
        if (table) retired.push_back(table);
        table = buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        table_bytes = bytes;
      }
      ComPtr<ID3D12Resource> up = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
      void* p = nullptr;
      up->Map(0, nullptr, &p);
      std::memcpy(p, words.data(), bytes);
      up->Unmap(0, nullptr);
      misses_at = words[4];
      misses_capacity = words[5];
      b.Transition = {table.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
      list->ResourceBarrier(1, &b);
      list->CopyBufferRegion(table.Get(), 0, up.Get(), 0, bytes);
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      list->ResourceBarrier(1, &b);
      retired.push_back(std::move(up));
      table_dirty = false;
      return;  // a fresh table has no misses to read
    }
    if (!table || readback_state != Readback::idle) return;
    // The misses so far go to the readback buffer; the count starts again at 0.
    const uint64_t miss_bytes = uint64_t{misses_capacity} * 48;
    if (!readback) readback = buffer(32 + miss_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!header_upload) {
      header_upload = buffer(4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
      void* p = nullptr;
      header_upload->Map(0, nullptr, &p);
      std::memset(p, 0, 4);
      header_upload->Unmap(0, nullptr);
    }
    b.Transition = {table.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(readback.Get(), 0, table.Get(), 0, 32);
    list->CopyBufferRegion(readback.Get(), 32, table.Get(), misses_at, miss_bytes);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(table.Get(), 24, header_upload.Get(), 0, 4);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &b);
    readback_state = Readback::recorded;
  }
};

Bindings& bindings() {
  static Bindings* const b = new Bindings;
  return *b;
}

Bindings* ready() {
  Bindings& b = bindings();
  if (!b.started) b.start();
  return b.usable ? &b : nullptr;
}

}  // namespace

uint32_t image_format(uint32_t dxgi) {
  switch (static_cast<DXGI_FORMAT>(dxgi)) {
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R8_SNORM: return 2;
    case DXGI_FORMAT_R8_UINT: return 5;
    case DXGI_FORMAT_R16_UNORM: return 7;
    case DXGI_FORMAT_R16_FLOAT: return 13;
    case DXGI_FORMAT_R8G8_UNORM: return 14;
    case DXGI_FORMAT_R32_UINT: return 20;
    case DXGI_FORMAT_R32_FLOAT: return 22;
    case DXGI_FORMAT_R16G16_FLOAT: return 29;
    case DXGI_FORMAT_R11G11B10_FLOAT: return 36;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 50;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return 56;
    case DXGI_FORMAT_R32G32_FLOAT: return 64;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 71;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 77;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 130;
    case DXGI_FORMAT_BC1_UNORM: return 169;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return 170;
    case DXGI_FORMAT_BC2_UNORM: return 171;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return 172;
    case DXGI_FORMAT_BC3_UNORM: return 173;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return 174;
    case DXGI_FORMAT_BC4_UNORM: return 175;
    case DXGI_FORMAT_BC4_SNORM: return 176;
    case DXGI_FORMAT_BC5_UNORM: return 177;
    case DXGI_FORMAT_BC5_SNORM: return 178;
    case DXGI_FORMAT_BC6H_UF16: return 179;
    case DXGI_FORMAT_BC6H_SF16: return 180;
    case DXGI_FORMAT_BC7_UNORM: return 181;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return 182;
    default: return 0;
  }
}

void bind_texture_view(const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view, uint64_t address,
                       uint32_t words[8]) {
  std::memset(words, 0, 32);
  const uint32_t format = view.Format ? view.Format : resource.Format;
  const uint32_t mips = std::max<uint32_t>(resource.MipLevels, 1);
  const uint32_t width = static_cast<uint32_t>(resource.Width), height = resource.Height;
  TextureLayout layout;
  if (!texture_layout(resource, layout)) return;
  // The T#: address >> 8, format, size, channel order (B8G8R8A8 reads its channels swapped), the
  // whole mip chain, the swizzle mode, 2D.
  const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  words[0] = static_cast<uint32_t>(address >> 8);
  words[1] = static_cast<uint32_t>(address >> 40) & 0xFF;
  words[1] |= (image_format(format) & 0x1FF) << 20;
  words[1] |= ((width - 1) & 3) << 30;
  words[2] = ((width - 1) >> 2) & 0x3FFF;
  words[2] |= ((height - 1) & 0xFFFF) << 14;
  words[2] |= 1u << 31;  // resource level
  const uint32_t sel = bgra ? (6u | (5u << 3) | (4u << 6) | (7u << 9)) : (4u | (5u << 3) | (6u << 6) | (7u << 9));
  words[3] = sel | ((mips - 1) << 16) | ((layout.swizzle_mode & 31) << 20) | (9u << 28);
  words[5] = ((mips - 1) & 15) << 4;
  if (resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resource.DepthOrArraySize != 1) return;  // 2D only for now

  std::lock_guard g(bindings().lock);
  Bindings* b = ready();
  if (!b) return;
  for (const auto& i : b->images) {
    if (std::equal(i.words, i.words + 8, words)) return;  // the same view again
  }
  Texture t;
  t.address = address;
  t.size = layout.size;
  t.layout = layout;
  t.desc = resource;
  D3D12_RESOURCE_DESC hd{};
  hd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  hd.Width = width;
  hd.Height = height;
  hd.DepthOrArraySize = 1;
  hd.MipLevels = static_cast<UINT16>(mips);
  hd.Format = resource.Format;
  hd.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(b->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&t.resource)))) {
    return;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE slot{};
  const uint32_t index = allocate_view_slot(&slot);
  if (!index) return;
  D3D12_SHADER_RESOURCE_VIEW_DESC v = view;
  if (!v.Format) v.Format = resource.Format;
  v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  v.Texture2D = {0, mips, 0, 0.0f};
  b->device->CreateShaderResourceView(t.resource.Get(), &v, slot);
  shader::ImageBinding ib;
  std::copy(words, words + 8, ib.words);
  ib.srv = ib.alt = index;
  ib.flags = shader::kImageFloat;
  b->images.push_back(ib);
  b->textures.push_back(std::move(t));
  b->table_dirty = true;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    logf("gpu: texture %ux%u, %u mips, format %u, mode %u, at 0x%s: view %u", width, height, mips, format, layout.swizzle_mode,
         hex(address).c_str(), index);
  }
}

void notify_title_write(uint64_t address, uint64_t size) {
  std::lock_guard g(bindings().lock);
  for (Texture& t : bindings().textures) {
    if (address < t.address + t.size && t.address < address + size) t.dirty = true;
  }
}

void flush_bindings(ID3D12GraphicsCommandList* list) {
  std::lock_guard g(bindings().lock);
  if (Bindings* b = ready()) b->flush(list);
}

void bindings_submitted(uint64_t fence_value) {
  std::lock_guard g(bindings().lock);
  Bindings& b = bindings();
  if (b.readback_state == Bindings::Readback::recorded) {
    b.readback_state = Bindings::Readback::in_flight;
    b.readback_fence = fence_value;
  }
}

D3D12_GPU_VIRTUAL_ADDRESS bindings_table() {
  std::lock_guard g(bindings().lock);
  Bindings* b = ready();
  return b && b->table ? b->table->GetGPUVirtualAddress() : 0;
}

ID3D12DescriptorHeap* sampler_heap() {
  std::lock_guard g(bindings().lock);
  Bindings* b = ready();
  return b ? b->samplers.Get() : nullptr;
}

}  // namespace am::rt::d3d12x
