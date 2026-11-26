#include "runtime/d3d12x/bindings.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/text.h"
#include "runtime/d3d12x/formats.h"
#include "runtime/d3d12x/gpu_memory.h"
#include "runtime/d3d12x/host.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/log.h"
#include "runtime/memory/write_watch.h"
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

// Title memory as the GPU sees it (gpu_bytes_at: through the GPU's own page mappings); bytes it
// cannot read are zero.
bool read_gpu(uint64_t address, void* to, uint64_t size) {
  bool ok = true;
  auto* out = static_cast<uint8_t*>(to);
  while (size) {
    uint64_t run = 0;
    const void* from = gpu_bytes_at(address, &run);
    const uint64_t n = std::min(size, std::max<uint64_t>(run, 1));
    if (from) {
      ok = read_guarded(from, out, n) && ok;
    } else {
      std::memset(out, 0, n);
    }
    address += n;
    out += n;
    size -= n;
  }
  return ok;
}

// GFX10's SW_4KB_S ("standard") tiling, the mode nearly every texture of the game uses: 4 KiB
// tiles, row by row across the surface; inside a tile the address bits interleave the element's
// x and y bits by element size (from AMD's address library patterns; 'z' is a byte within the
// element). 1 to 16-byte elements; BC formats count 4x4 blocks as elements.
//
// The patterns below derive from AMD's address library (addrlib, gfx10SwizzlePattern.h),
// Copyright (c) 2007-2024 Advanced Micro Devices, Inc. All Rights Reserved. Used under the MIT
// licence: permission is granted free of charge to use, copy, modify, merge, publish and
// distribute, provided this notice is included; the software is provided "as is", without
// warranty of any kind.
constexpr uint32_t kSw4kS = 5;
constexpr const char* kPattern4kS[5] = {
    "x0x1x2x3y0y1y2y3y4x4y5x5",  // 1 byte
    "z0x0x1x2y0y1y2x3y3x4y4x5",  // 2 bytes
    "z0z1x0x1y0y1y2x2y3x3y4x4",  // 4 bytes
    "z0z1z2x0y0y1x1x2y2x3y3x4",  // 8 bytes
    "z0z1z2z3y0y1x0x1y2x2y3x3",  // 16 bytes
};

int element_log2(uint32_t bytes) {
  for (int k = 0; k < 5; ++k) {
    if (bytes == (1u << k)) return k;
  }
  return -1;
}

// The byte offset of element (x, y) in a surface `pitch` elements wide.
uint64_t offset_4k_s(int log2_bytes, uint32_t x, uint32_t y, uint32_t pitch) {
  const char* pat = kPattern4kS[log2_bytes];
  uint32_t in_tile = 0, xbits = 0, ybits = 0;
  for (int bit = 0; bit < 12; ++bit) {
    const char axis = pat[2 * bit];
    const int n = pat[2 * bit + 1] - '0';
    if (axis == 'x') {
      in_tile |= ((x >> n) & 1u) << bit;
      xbits = std::max<uint32_t>(xbits, n + 1);
    } else if (axis == 'y') {
      in_tile |= ((y >> n) & 1u) << bit;
      ybits = std::max<uint32_t>(ybits, n + 1);
    }
  }
  const uint32_t tile_w = 1u << xbits, tile_h = 1u << ybits;
  const uint64_t tiles_per_row = (pitch + tile_w - 1) / tile_w;
  return ((uint64_t{y / tile_h} * tiles_per_row + x / tile_w) << 12) + in_tile;
}

// The texture watcher's rounds (see Bindings::watch): a texture that changed is checked every
// round for kLiveRounds rounds after; a new one for kNewRounds (the title often fills it after
// making its view, which is when it was first uploaded).
constexpr uint32_t kLiveRounds = 600, kNewRounds = 200;

struct Texture {
  uint64_t address = 0, size = 0;
  D3D12_RESOURCE_DESC desc{};
  TextureLayout layout;
  ComPtr<ID3D12Resource> resource;
  bool dirty = true;
  uint64_t checksum = 0;  // of its bytes in title memory when last uploaded
  // Rounds in a row that found it unchanged since it last changed; kLiveRounds and under: live,
  // checked every round.
  uint32_t quiet = kLiveRounds - kNewRounds;
  // Its memory is watched for writes (mem::watch_writes) since its last upload: neither hashed
  // nor checked; a write marks it at the next submission. Watched where the title's CPU writes
  // its bytes (gpu_write_ranges), when all of that is private read-write memory; textures in
  // physical pages (the GPU's own mappings place those) are hashed instead.
  bool watched = false;
  std::vector<std::pair<uint64_t, uint64_t>> watch_ranges;
  uint32_t view = 0;  // its view slot (the table's srv index)
};

// A quick checksum of title memory (0 when it cannot be read). Not every write to a texture's
// memory passes through a path we see (the CPU, DirectStorage into physical pages), so the
// textures are checked against their memory from time to time.
// Four lanes, so it runs at memory speed rather than at one multiply's latency a word.
uint64_t checksum_span(uint64_t h, const uint64_t* p, uint64_t words) {
  constexpr uint64_t kOdd = 0x9E3779B97F4A7C15ull;
  uint64_t a = h, b = h ^ 0x6A09E667F3BCC909ull, c = h ^ 0xBB67AE8584CAA73Bull, d = h ^ 0x3C6EF372FE94F82Bull;
  __try {
    uint64_t k = 0;
    for (; k + 4 <= words; k += 4) {
      a = _rotl64((a ^ p[k]) * kOdd, 29);
      b = _rotl64((b ^ p[k + 1]) * kOdd, 29);
      c = _rotl64((c ^ p[k + 2]) * kOdd, 29);
      d = _rotl64((d ^ p[k + 3]) * kOdd, 29);
    }
    for (; k < words; ++k) a = _rotl64((a ^ p[k]) * kOdd, 29);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  h = (a ^ _rotl64(b, 17) ^ _rotl64(c, 31) ^ _rotl64(d, 47)) * kOdd;
  h ^= h >> 29;
  return h ? h : 1;  // 0 means unreadable
}

uint64_t checksum_of(uint64_t address, uint64_t size) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (uint64_t at = address, end = address + size / 8 * 8; at < end;) {
    uint64_t run = 0;
    const void* from = gpu_bytes_at(at, &run);
    const uint64_t n = std::min(end - at, std::max<uint64_t>(run, 8)) / 8 * 8;
    if (from) h = checksum_span(h, static_cast<const uint64_t*>(from), n / 8);
    if (!h) return 0;
    h += n;
    at += n;
  }
  return h;
}

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

struct Bindings;
void add_texture(Bindings* b, const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view, uint64_t address,
                 const uint32_t words[8], ID3D12Resource* host, const TextureLayout& layout);
bool texture_from_descriptor(const uint32_t words[8], D3D12_RESOURCE_DESC& desc, uint64_t& address);

struct Bindings {
  std::mutex lock;
  bool started = false, usable = false;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12DescriptorHeap> samplers;
  UINT sampler_step = 0, next_sampler = 0;
  std::vector<shader::ImageBinding> images;
  std::vector<shader::SamplerBinding> sampler_bindings;
  std::vector<Texture> textures;
  // Copies of render targets in another format (see target_has_copies).
  struct TargetCopy {
    ComPtr<ID3D12Resource> target, copy;
  };
  std::vector<TargetCopy> target_copies;
  bool table_dirty = true;
  ComPtr<ID3D12Resource> table, upload, readback, header_upload;
  uint64_t table_bytes = 0;
  uint32_t misses_at = 0, misses_capacity = 0;
  // The misses' readback: recorded into a list, then submitted (with the fence value after it).
  enum class Readback { idle, recorded, in_flight } readback_state = Readback::idle;
  uint64_t readback_fence = 0;
  // Buffers the GPU may still read (uploads, a replaced table), each with the fence value after
  // the lists that use them; kPending until those lists are submitted.
  static constexpr uint64_t kPending = UINT64_MAX;
  struct Retired {
    uint64_t fence;
    const void* list;  // the list that reads it
    ComPtr<ID3D12Resource> resource;
    uint64_t bytes = 0;  // its size (upload memory in flight)
  };
  static constexpr uint64_t kUploadBudget = uint64_t{512} << 20;
  uint64_t uploads_deferred = 0;
  std::vector<Retired> retired;
  // Textures dropped because the title's memory under them now holds another texture: their
  // resource and view slot go once the GPU is past a fence signalled 3 frames later (lists
  // recorded before the drop may still read them).
  struct Evicted {
    uint64_t frame = 0, fence = 0;
    ComPtr<ID3D12Resource> resource;
    uint32_t view = 0;
  };
  std::vector<Evicted> evicted;
  uint64_t frames = 0, evictions = 0;
  const void* recording = nullptr;  // the list being flushed into
  const void* readback_list = nullptr;

  bool start() {
    started = true;
    present::HostGpu* gpu = present::host_gpu();
    if (!gpu) return false;
    device = gpu->device;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&samplers)))) return false;
    sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    usable = true;
    std::thread([this] { watch(); }).detach();
    return true;
  }

  ComPtr<ID3D12Resource> buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r));
    if (r) count_host_memory(HostMemoryKind::Buffer, r.Get());
    if (!r) {
      static std::atomic<int> told{0};
      if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
        logf("gpu: no %s buffer of %s for the bindings (0x%08lx)", type == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "GPU",
             format_bytes(size).c_str(), static_cast<unsigned long>(device->GetDeviceRemovedReason()));
      }
    }
    if (r) r->SetName(type == D3D12_HEAP_TYPE_UPLOAD ? L"bindings upload" : type == D3D12_HEAP_TYPE_READBACK ? L"bindings readback" : L"bindings table");
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
      else if (r[0] == 1 && add_from_descriptor(r + 4)) {
      } else if (r[0] == 1 && unknown_images.fetch_add(1) < 8) {
        logf("gpu: a shader used a texture we did not make (T# %08x %08x %08x %08x %08x %08x %08x %08x)", r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11]);
      }
    }
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
  }

  // A texture the title described itself (it wrote the T# into memory, no view call of ours):
  // made from the descriptor's own fields.
  bool add_from_descriptor(const uint32_t words[8]) {
    for (const auto& i : images) {
      if (std::equal(i.words, i.words + 8, words)) return true;
    }
    D3D12_RESOURCE_DESC desc{};
    uint64_t address = 0;
    TextureLayout layout;
    if (!texture_from_descriptor(words, desc, address) || !texture_layout(desc, layout)) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    const size_t before = images.size();
    add_texture(this, desc, view, address, words, nullptr, layout);
    static std::atomic<int> told{0};
    if (images.size() > before && told.fetch_add(1, std::memory_order_relaxed) < 16) {
      logf("gpu: texture from the title's own descriptor: %llux%u, %u mips, format %u, mode %u, at 0x%s",
           static_cast<unsigned long long>(desc.Width), desc.Height, desc.MipLevels, desc.Format, layout.swizzle_mode,
           hex(address).c_str());
    }
    return images.size() > before;
  }

  void upload_texture(ID3D12GraphicsCommandList* list, Texture& t) {
    t.dirty = false;
    // Watched before it is read: a write from here on is seen.
    static const bool watch = GetEnvironmentVariableA("AMERICAS_NO_WRITE_WATCH", nullptr, 0) == 0;
    t.watch_ranges.clear();
    t.watched = watch && gpu_write_ranges(t.address, t.size, t.watch_ranges) &&
                std::all_of(t.watch_ranges.begin(), t.watch_ranges.end(),
                            [](const auto& r) { return mem::watch_writes(r.first, r.second - r.first); });
    t.checksum = t.watched ? 0 : checksum_of(t.address, t.size);
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
    const int log2_bytes = element_log2(t.layout.element_bytes);
    const bool detile = t.layout.swizzle_mode == kSw4kS && log2_bytes >= 0;
    std::vector<uint8_t> tiled;
    for (UINT k = 0; k < subs; ++k) {
      const SubresourceLayout& s = t.layout.subresources[k];
      const uint64_t bytes = std::min<uint64_t>(row_bytes[k], s.row_pitch);
      if (s.in_tail && fp[k].Footprint.Depth == 1) {
        // In the mip tail: the tail block, the mip from (tail_x, tail_y) of it (in the 4 KiB
        // standard order, or row by row in the block for the modes we keep linear).
        const uint64_t size = t.layout.block_bytes;
        tiled.resize(size);
        read_gpu(t.address + s.offset, tiled.data(), size);
        const uint32_t eb = t.layout.element_bytes, pitch = t.layout.block_width;
        for (UINT y = 0; y < rows[k] && y < s.rows; ++y) {
          uint8_t* row = p + fp[k].Offset + uint64_t{y} * fp[k].Footprint.RowPitch;
          for (uint32_t x = 0; x < s.width_elements && (x + 1) * eb <= row_bytes[k]; ++x) {
            const uint32_t ex = s.tail_x + x, ey = s.tail_y + y;
            const uint64_t at = detile ? offset_4k_s(log2_bytes, ex, ey, pitch) : (uint64_t{ey} * pitch + ex) * eb;
            if (at + eb <= size) std::memcpy(row + uint64_t{x} * eb, tiled.data() + at, eb);
          }
        }
        continue;
      }
      if (detile && fp[k].Footprint.Depth == 1) {
        // The subresource's tiles, then each element from its place in its 4 KiB tile.
        const uint64_t size = uint64_t{s.row_pitch} * s.padded_rows;
        tiled.resize(size);
        read_gpu(t.address + s.offset, tiled.data(), size);
        const uint32_t eb = t.layout.element_bytes, pitch = s.row_pitch / eb;
        for (UINT y = 0; y < rows[k] && y < s.rows; ++y) {
          uint8_t* row = p + fp[k].Offset + uint64_t{y} * fp[k].Footprint.RowPitch;
          for (uint32_t x = 0; x < s.width_elements && (x + 1) * eb <= row_bytes[k]; ++x) {
            const uint64_t at = offset_4k_s(log2_bytes, x, y, pitch);
            if (at + eb <= size) std::memcpy(row + uint64_t{x} * eb, tiled.data() + at, eb);
          }
        }
        continue;
      }
      for (UINT z = 0; z < fp[k].Footprint.Depth; ++z) {
        for (UINT y = 0; y < rows[k] && y < s.rows; ++y) {
          const uint64_t from = t.address + s.offset + (uint64_t{z} * s.padded_rows + y) * s.row_pitch;
          read_gpu(from, p + fp[k].Offset + (uint64_t{z} * rows[k] + y) * fp[k].Footprint.RowPitch, bytes);
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
    const uint64_t up_bytes = up->GetDesc().Width;
    retired.push_back({kPending, recording, std::move(up), up_bytes});
  }

  // Textures whose memory changed since their upload are uploaded again. Live ones (new, or
  // changed lately: video frames, glyph atlases) are checked at every submission, right before
  // the title's lists run (check_live), so what the title wrote before submitting is what they
  // read. The others are checked by the watcher, a thread of its own: each round hashes up to
  // kRoundBytes of them in turn with the lock released, then sleeps a little; the whole set
  // (420 MiB in the menus) goes by in about 200 ms. A texture it finds changed turns live.
  static constexpr uint64_t kRoundBytes = 48ull << 20;
  size_t next_check = 0;
  struct Watched {
    size_t index;
    uint64_t address, size, checksum, now;
  };

  void watch() {
    std::vector<Watched> round;
    for (;;) {
      round.clear();
      {
        std::lock_guard g(lock);
        const size_t count = textures.size();
        uint64_t budget = kRoundBytes;
        for (size_t n = 0; n < count && budget; ++n) {
          const size_t i = next_check++ % count;
          const Texture& t = textures[i];
          if (t.dirty || t.watched || !t.resource || t.quiet <= kLiveRounds) continue;
          budget -= std::min(t.size, budget);
          round.push_back({i, t.address, t.size, t.checksum, 0});
        }
      }
      for (Watched& w : round) w.now = checksum_of(w.address, w.size);
      {
        std::lock_guard g(lock);
        for (const Watched& w : round) {
          // Textures may have been dropped meanwhile: the index must still name this one.
          if (w.index >= textures.size() || textures[w.index].address != w.address || textures[w.index].size != w.size) continue;
          Texture& t = textures[w.index];
          if (t.dirty || t.watched || t.checksum != w.checksum) continue;  // uploaded again meanwhile
          if (w.now != w.checksum) {
            t.dirty = true;
            t.quiet = 0;
          }
        }
      }
      Sleep(round.empty() ? 50 : 12);
    }
  }

  // At a submission, under the lock: watched textures the title wrote, and live ones whose
  // memory changed, are marked.
  std::vector<std::pair<uint64_t, uint64_t>> written_ranges;
  void check_live() {
    // Watched textures the title wrote.
    written_ranges.clear();
    mem::take_written(written_ranges);
    for (const auto& [first, end] : written_ranges) {
      for (Texture& t : textures) {
        if (t.watched && std::any_of(t.watch_ranges.begin(), t.watch_ranges.end(),
                                     [&](const auto& r) { return r.first < end && first < r.second; })) {
          t.watched = false;
          t.dirty = true;
        }
      }
    }
    for (Texture& t : textures) {
      if (t.dirty || t.watched || !t.resource || t.quiet > kLiveRounds) continue;
      if (checksum_of(t.address, t.size) != t.checksum) {
        t.dirty = true;
        t.quiet = 0;
      } else {
        ++t.quiet;
      }
    }
  }

  // The uploads' own lists, run ahead of the title's in the same submission; each reused once
  // the GPU is past it.
  struct UploadList {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fence = kPending;
  };
  std::vector<UploadList> upload_lists;

  // Lets go of the buffers the GPU is done with; logs the ones waiting as they grow.
  void reclaim() {
    present::HostGpu* gpu = present::host_gpu();
    std::erase_if(retired, [gpu](const Retired& r) { return r.fence != kPending && gpu->done(r.fence); });
    for (auto it = evicted.begin(); it != evicted.end();) {
      if (!it->fence && frames >= it->frame + 3) it->fence = gpu->signal();
      if (it->fence && gpu->done(it->fence)) {
        release_view_slot(it->view);
        it = evicted.erase(it);
      } else {
        ++it;
      }
    }
    static size_t told = 64;
    if (retired.size() >= told) {
      uint64_t bytes = 0, pending = 0;
      for (const Retired& r : retired) {
        bytes += r.resource ? r.resource->GetDesc().Width : 0;
        pending += r.fence == kPending;
      }
      logf("bindings: %zu buffers waiting for the GPU (%llu MiB, %llu not yet submitted)", retired.size(),
           static_cast<unsigned long long>(bytes >> 20), static_cast<unsigned long long>(pending));
      told *= 2;
    }
  }

  ID3D12CommandList* uploads_for_submission() {
    if (!usable) return nullptr;
    reclaim();
    check_live();
    if (std::none_of(textures.begin(), textures.end(), [](const Texture& t) { return t.dirty && t.resource; })) return nullptr;
    present::HostGpu* gpu = present::host_gpu();
    UploadList* u = nullptr;
    for (UploadList& c : upload_lists) {
      if (c.fence != kPending && gpu->done(c.fence)) {
        u = &c;
        break;
      }
    }
    if (u) {
      if (FAILED(u->allocator->Reset()) || FAILED(u->list->Reset(u->allocator.Get(), nullptr))) return nullptr;
    } else {
      UploadList c;
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator))) ||
          FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator.Get(), nullptr, IID_PPV_ARGS(&c.list)))) {
        return nullptr;
      }
      c.list->SetName(L"texture uploads");
      upload_lists.push_back(std::move(c));
      u = &upload_lists.back();
    }
    u->fence = kPending;
    recording = u->list.Get();
    // Upload memory (system memory, one buffer an upload) waiting for the GPU is bounded: past
    // kUploadBudget the remaining textures stay dirty for a later submission. Unbounded, it
    // reached gigabytes while the GPU was busy, and the process's commit with it.
    uint64_t in_flight = 0;
    for (const Retired& r : retired) in_flight += r.bytes;
    size_t deferred = 0;
    for (Texture& t : textures) {
      if (!t.dirty || !t.resource) continue;
      if (in_flight >= kUploadBudget) {
        ++deferred;
        continue;
      }
      const size_t before = retired.size();
      upload_texture(u->list.Get(), t);
      for (size_t k = before; k < retired.size(); ++k) in_flight += retired[k].bytes;
    }
    if (deferred) {
      ++uploads_deferred;
      static std::atomic<int> told{0};
      if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
        logf("bindings: %zu texture uploads wait for the GPU (%s of uploads in flight)", deferred, format_bytes(in_flight).c_str());
      }
    }
    if (FAILED(u->list->Close())) return nullptr;
    return u->list.Get();
  }

  void flush(ID3D12GraphicsCommandList* list) {
    if (!usable) return;
    // Freed once the GPU has run the lists that read them.
    reclaim();
    recording = list;
    take_misses();
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    if (table_dirty) {
      std::vector<uint32_t> words = shader::build_bindings(images, sampler_bindings, 256);
      if (words.empty()) return;
      const uint64_t bytes = words.size() * 4;
      if (!table || table_bytes < bytes) {
        ComPtr<ID3D12Resource> grown =
            buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!grown) return;  // the old table stays (and stays dirty): tried again at the next flush
        if (table) retired.push_back({kPending, recording, table, table->GetDesc().Width});
        table = std::move(grown);
        table_bytes = bytes;
      }
      ComPtr<ID3D12Resource> up = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
      void* p = nullptr;
      if (!up || FAILED(up->Map(0, nullptr, &p))) return;
      std::memcpy(p, words.data(), bytes);
      up->Unmap(0, nullptr);
      misses_at = words[4];
      misses_capacity = words[5];
      b.Transition = {table.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
      list->ResourceBarrier(1, &b);
      list->CopyBufferRegion(table.Get(), 0, up.Get(), 0, bytes);
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      list->ResourceBarrier(1, &b);
      const uint64_t up_bytes = up->GetDesc().Width;
      retired.push_back({kPending, recording, std::move(up), up_bytes});
      table_dirty = false;
      return;  // a fresh table has no misses to read
    }
    if (!table || readback_state != Readback::idle) return;
    // The misses so far go to the readback buffer; the count starts again at 0.
    const uint64_t miss_bytes = uint64_t{misses_capacity} * 48;
    if (!readback) readback = buffer(32 + miss_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!readback) return;
    if (!header_upload) {
      header_upload = buffer(4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
      void* p = nullptr;
      if (!header_upload || FAILED(header_upload->Map(0, nullptr, &p))) {
        header_upload.Reset();
        return;
      }
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
    readback_list = list;
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

// A texture view the host accepts, from the title's: depth formats as their readable twins,
// stencil views on plane 1 (and read as unsigned integers), and only formats the device can
// read in shaders. False when there is none: an invalid view would remove the device.
bool host_view_desc(ID3D12Device* device, DXGI_FORMAT resource_format, D3D12_SHADER_RESOURCE_VIEW_DESC& v, UINT mips,
                    uint32_t& image_flags) {
  DXGI_FORMAT f = v.Format ? v.Format : resource_format;
  UINT plane = 0;
  image_flags = shader::kImageFloat;
  switch (f) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: f = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: f = DXGI_FORMAT_R16_UNORM; break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      plane = 1;
      image_flags = shader::kImageUint;
      break;
    default: break;
  }
  // A depth-stencil texture read as its depth: the view format its family takes.
  if (plane == 0 && (resource_format == DXGI_FORMAT_R32G8X24_TYPELESS || resource_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) &&
      (f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS)) {
    f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  }
  if (plane == 0 && (resource_format == DXGI_FORMAT_R24G8_TYPELESS || resource_format == DXGI_FORMAT_D24_UNORM_S8_UINT) &&
      (f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS)) {
    f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  }
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f, {}, {}};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) ||
      !(support.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))) {
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 8) {
      logf("gpu: no shader view of format %u (texture format %u): not one the host reads", f, resource_format);
    }
    return false;
  }
  v.Format = f;
  v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  v.Texture2D = {0, mips, plane, 0.0f};
  return true;
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

namespace {

// The texture an RDNA 2 image descriptor (T#) describes: 2D, one slice, from its first mip.
bool texture_from_descriptor(const uint32_t w[8], D3D12_RESOURCE_DESC& desc, uint64_t& address) {
  const uint32_t type = w[3] >> 28, base_level = (w[3] >> 12) & 15, last_level = (w[3] >> 16) & 15;
  const uint32_t image = (w[1] >> 20) & 0x1FF, mode = (w[3] >> 20) & 31, depth = w[4] & 0x1FFF;
  if (type != 9 || base_level || depth) return false;  // 2D, whole chain, one slice: the rest later
  address = (uint64_t{w[0]} | (uint64_t{w[1] & 0xFF} << 32)) << 8;
  const uint32_t width = (((w[2] & 0x3FFF) << 2) | (w[1] >> 30)) + 1, height = ((w[2] >> 14) & 0xFFFF) + 1;
  // The DXGI format with this image format; channels swapped (z y x w) means B8G8R8A8.
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  for (uint32_t f = 1; f < 120 && !format; ++f) {
    if (image_format(f) == image) format = static_cast<DXGI_FORMAT>(f);
  }
  if (!format || !address || !width || !height) return false;
  const bool swapped = (w[3] & 0xFFF) == (6u | (5u << 3) | (4u << 6) | (7u << 9));
  if (swapped && format == DXGI_FORMAT_R8G8B8A8_UNORM) format = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (swapped && format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<UINT16>(last_level + 1);
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kLayoutSwizzleFlag | mode);
  return true;
}

}  // namespace

void bind_texture_view(const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view, uint64_t address,
                       uint32_t words[8], ID3D12Resource* host) {
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
  add_texture(b, resource, view, address, words, host, layout);
}

namespace {

// Under the bindings' lock: the table entry for `words`, with its host texture (or `host`'s own).
void add_texture(Bindings* b, const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view, uint64_t address,
                 const uint32_t words[8], ID3D12Resource* host, const TextureLayout& layout) {
  const uint32_t format = view.Format ? view.Format : resource.Format;
  const uint32_t mips = std::max<uint32_t>(resource.MipLevels, 1);
  const uint32_t width = static_cast<uint32_t>(resource.Width), height = resource.Height;
  for (const auto& i : b->images) {
    if (std::equal(i.words, i.words + 8, words)) return;  // the same view again
  }
  if (host) {
    // A texture the GPU renders into: its own view, no copy from title memory.
    if (host->GetDesc().SampleDesc.Count > 1) return;  // multisampled: a view of its own kind, not yet
    D3D12_CPU_DESCRIPTOR_HANDLE slot{};
    const uint32_t index = allocate_view_slot(&slot);
    if (!index) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC v = view;
    uint32_t image_flags = 0;
    if (!host_view_desc(b->device.Get(), host->GetDesc().Format, v, mips, image_flags)) return;
    b->device->CreateShaderResourceView(host, &v, slot);
    shader::ImageBinding ib;
    std::copy(words, words + 8, ib.words);
    ib.srv = ib.alt = index;
    ib.flags = image_flags;
    // A texture shaders write: a writable view of its first mip too (typed stores need a format
    // the device stores; sRGB views write through their UNORM twin).
    if (host->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
      DXGI_FORMAT f = v.Format;
      if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) f = DXGI_FORMAT_R8G8B8A8_UNORM;
      if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) f = DXGI_FORMAT_B8G8R8A8_UNORM;
      D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f, {}, {}};
      D3D12_CPU_DESCRIPTOR_HANDLE uav_slot{};
      if (SUCCEEDED(b->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) &&
          (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        if (const uint32_t uav = allocate_view_slot(&uav_slot)) {
          D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
          u.Format = f;
          u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
          b->device->CreateUnorderedAccessView(host, nullptr, &u, uav_slot);
          ib.uav = uav;
          ib.flags |= shader::kImageHasUav;
        }
      }
    }
    b->images.push_back(ib);
    b->table_dirty = true;
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 16) {
      logf("gpu: texture %ux%u, format %u, at 0x%s: the GPU's own (a render target): view %u", width, height, format,
           hex(address).c_str(), index);
    }
    return;
  }
  // The memory of a render target, viewed in another format of the same size: a texture of that
  // format, which the target is copied into after each pass that draws into it (the GPU never
  // writes title memory, and viewing the target itself in that format is not allowed).
  D3D12_RESOURCE_DESC target_desc{};
  if (ID3D12Resource* target = host_texture_at(address, &target_desc)) {
    const D3D12_RESOURCE_DESC td = target->GetDesc();
    const FormatSize a = format_size(td.Format), c = format_size(resource.Format);
    if (td.Width == resource.Width && td.Height == resource.Height && td.SampleDesc.Count == 1 && a.bytes && a.bytes == c.bytes &&
        a.block == 1 && c.block == 1 && td.Format != resource.Format) {
      D3D12_RESOURCE_DESC cd{};
      cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      cd.Width = width;
      cd.Height = height;
      cd.DepthOrArraySize = 1;
      cd.MipLevels = 1;
      cd.Format = resource.Format;
      cd.SampleDesc.Count = 1;
      D3D12_HEAP_PROPERTIES chp{};
      chp.Type = D3D12_HEAP_TYPE_DEFAULT;
      ComPtr<ID3D12Resource> copy;
      if (FAILED(b->device->CreateCommittedResource(&chp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&copy)))) {
        return;
      }
      copy->SetName(widen("copy of a target, format " + std::to_string(resource.Format) + " at 0x" + hex(address)).c_str());
      count_host_memory(HostMemoryKind::TargetCopy, copy.Get());
      D3D12_CPU_DESCRIPTOR_HANDLE slot{};
      const uint32_t index = allocate_view_slot(&slot);
      if (!index) return;
      D3D12_SHADER_RESOURCE_VIEW_DESC v = view;
      uint32_t image_flags = 0;
      if (!host_view_desc(b->device.Get(), resource.Format, v, 1, image_flags)) return;
      b->device->CreateShaderResourceView(copy.Get(), &v, slot);
      shader::ImageBinding ib;
      std::copy(words, words + 8, ib.words);
      ib.srv = ib.alt = index;
      ib.flags = image_flags;
      b->images.push_back(ib);
      b->target_copies.push_back({target, std::move(copy)});
      b->table_dirty = true;
      logf("gpu: texture %ux%u, format %u, at 0x%s: a copy of the render target there (format %u): view %u", width, height,
           resource.Format, hex(address).c_str(), td.Format, index);
      return;
    }
  }
  // Textures the new one's memory overlaps, unless they start where it starts (the same
  // texture viewed with another mip count or format: dropping those made them replace each other
  // over and over): the title has put something else there, they are dropped.
  for (auto it = b->textures.begin(); it != b->textures.end();) {
    const bool overlaps = address < it->address + it->size && it->address < address + layout.size;
    if (!overlaps || it->address == address) {
      ++it;
      continue;
    }
    const uint32_t view_index = it->view;
    std::erase_if(b->images, [view_index](const shader::ImageBinding& i) { return i.srv == view_index; });
    b->evicted.push_back({b->frames, 0, std::move(it->resource), view_index});
    ++b->evictions;
    it = b->textures.erase(it);
    b->table_dirty = true;
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
  t.resource->SetName(widen("texture " + std::to_string(width) + "x" + std::to_string(height) + " from 0x" + hex(address)).c_str());
  count_host_memory(HostMemoryKind::Texture, t.resource.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE slot{};
  const uint32_t index = allocate_view_slot(&slot);
  if (!index) return;
  D3D12_SHADER_RESOURCE_VIEW_DESC v = view;
  uint32_t image_flags = 0;
  if (!host_view_desc(b->device.Get(), resource.Format, v, mips, image_flags)) return;
  b->device->CreateShaderResourceView(t.resource.Get(), &v, slot);
  shader::ImageBinding ib;
  std::copy(words, words + 8, ib.words);
  ib.srv = ib.alt = index;
  ib.flags = image_flags;
  b->images.push_back(ib);
  t.view = index;
  b->textures.push_back(std::move(t));
  b->table_dirty = true;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 200) {
    logf("gpu: texture %ux%u, %u mips, format %u, mode %u, at 0x%s: view %u", width, height, mips, format, layout.swizzle_mode,
         hex(address).c_str(), index);
  }
}

}  // namespace

void notify_title_write(uint64_t address, uint64_t size, bool live) {
  std::lock_guard g(bindings().lock);
  for (Texture& t : bindings().textures) {
    if (address < t.address + t.size && t.address < address + size) {
      t.dirty = true;
      if (live) t.quiet = 0;
    }
  }
}

bool target_has_copies(ID3D12Resource* target) {
  std::lock_guard g(bindings().lock);
  for (const auto& c : bindings().target_copies) {
    if (c.target.Get() == target) return true;
  }
  return false;
}

void copy_target(ID3D12GraphicsCommandList* list, ID3D12Resource* target) {
  std::lock_guard g(bindings().lock);
  for (const auto& c : bindings().target_copies) {
    if (c.target.Get() != target) continue;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {c.copy.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{c.copy.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
  }
}

void flush_bindings(ID3D12GraphicsCommandList* list) {
  std::lock_guard g(bindings().lock);
  if (Bindings* b = ready()) b->flush(list);
}

ID3D12CommandList* texture_uploads_for_submission() {
  std::lock_guard g(bindings().lock);
  Bindings* b = ready();
  return b ? b->uploads_for_submission() : nullptr;
}

void bindings_submitted(uint64_t fence_value, const void* list) {
  std::lock_guard g(bindings().lock);
  Bindings& b = bindings();
  for (auto& u : b.upload_lists) {
    if (u.fence == Bindings::kPending && u.list.Get() == list) u.fence = fence_value;
  }
  for (auto& r : b.retired) {
    if (r.fence == Bindings::kPending && r.list == list) r.fence = fence_value;
  }
  if (b.readback_state == Bindings::Readback::recorded && b.readback_list == list) {
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

namespace {
std::atomic<uint64_t> g_host_bytes[static_cast<int>(HostMemoryKind::Count)];
std::atomic<uint64_t> g_host_count[static_cast<int>(HostMemoryKind::Count)];
}  // namespace

void count_host_memory(HostMemoryKind kind, ID3D12Resource* resource) {
  if (!resource) return;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device)))) return;
  const D3D12_RESOURCE_DESC d = resource->GetDesc();
  const D3D12_RESOURCE_ALLOCATION_INFO a = device->GetResourceAllocationInfo(0, 1, &d);
  g_host_bytes[static_cast<int>(kind)] += a.SizeInBytes;
  ++g_host_count[static_cast<int>(kind)];
}

void bindings_frame_ended() {
  std::lock_guard g(bindings().lock);
  ++bindings().frames;
}

std::string host_memory_counts() {
  static const char* const kNames[] = {"textures", "targets", "target copies", "buffers"};
  std::string text;
  {
    std::lock_guard g(bindings().lock);
    uint64_t in_flight = 0;
    for (const auto& r : bindings().retired) in_flight += r.bytes;
    text = "textures alive " + std::to_string(bindings().textures.size()) + ", dropped " + std::to_string(bindings().evictions) +
           " (" + std::to_string(bindings().evicted.size()) + " not yet released), uploads in flight " + format_bytes(in_flight) +
           " (deferred " + std::to_string(bindings().uploads_deferred) + " times); ";
  }
  for (int k = 0; k < static_cast<int>(HostMemoryKind::Count); ++k) {
    text += (k ? ", " : "") + std::string(kNames[k]) + " " + std::to_string(g_host_count[k].load()) + " (" + format_bytes(g_host_bytes[k].load()) + ")";
  }
  return text;
}

}  // namespace am::rt::d3d12x
