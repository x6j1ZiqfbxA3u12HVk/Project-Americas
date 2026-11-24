// xg_xs.dll, the console's texture layout library. The game imports two functions:
//
//   XGSuggestSwizzleMode(description, flags, &layout): fills a D3D12 description's Layout field
//     (0x100 plus a swizzle mode) for textures whose headers name no mode;
//   XGCreateTextureComputer(description, &computer): an object answering a texture's layout.
//
// Both describe textures by D3D12's resource description. The computer has no IUnknown: slot 0
// is AddRef and slot 1 Release. The game calls slot 8 with (texture memory, plane, subresource,
// linear data, row pitch, slice pitch): the copy of linear data into the texture's layout. Its
// other slots are probes until the game's calls show them. Every answer comes from layout.h, the
// model GetResourceAllocationInfo uses.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

struct TextureComputer : Object {
  D3D12_RESOURCE_DESC desc{};
  TextureLayout layout;
};

void log_desc(const char* what, const D3D12_RESOURCE_DESC& d, uint32_t mode) {
  logf("xg: %s(dim %u, %llux%u x%u, %u mips, format %u, %u samples, layout 0x%x, flags 0x%x, alignment %llu): mode %u",
       what, d.Dimension, static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, d.MipLevels, d.Format,
       d.SampleDesc.Count, static_cast<uint32_t>(d.Layout), d.Flags, static_cast<unsigned long long>(d.Alignment), mode);
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
  for (uint32_t z = 0; z < s.depth; ++z) {
    for (uint32_t y = 0; y < s.rows; ++y) {
      std::memcpy(to + z * slice_bytes + uint64_t{y} * s.row_pitch, from + uint64_t{z} * slice_pitch + uint64_t{y} * row_pitch,
                  std::min<uint64_t>(row_bytes, row_pitch ? row_pitch : row_bytes));
    }
  }
  return S_OK;
}

const Kind& computer_kind() {
  static const Kind k("XG texture computer", 32,
                      {
                          {0, "AddRef", reinterpret_cast<void*>(&add_ref)},
                          {1, "Release", reinterpret_cast<void*>(&release)},
                          {2, nullptr, kProbe},
                          {8, "CopyIntoSubresource", reinterpret_cast<void*>(&copy_into_subresource)},
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
