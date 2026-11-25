#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/text.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/host.h"
#include "runtime/log.h"
#include "runtime/present/host_gpu.h"

namespace am::rt::d3d12x {
namespace {

using Microsoft::WRL::ComPtr;

constexpr D3D12_RESOURCE_FLAGS kGpuWritten =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

// The depth format a typeless (or shader-view) format stands for.
DXGI_FORMAT depth_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_D16_UNORM;
    default: return f;
  }
}

// A typed format for render-target views of typeless resources: the family's UNORM / FLOAT.
DXGI_FORMAT typed_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    default: return f;
  }
}

// A descriptor heap handed out slot by slot; slots of dropped textures are reused.
class ViewHeap {
 public:
  explicit ViewHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT size) : type_(type), size_(size) {}
  D3D12_CPU_DESCRIPTOR_HANDLE take(ID3D12Device* device) {
    if (!heap_) {
      D3D12_DESCRIPTOR_HEAP_DESC d{type_, size_, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap_)))) return {};
      start_ = heap_->GetCPUDescriptorHandleForHeapStart();
      stride_ = device->GetDescriptorHandleIncrementSize(type_);
    }
    UINT slot;
    if (!free_.empty()) {
      slot = free_.back();
      free_.pop_back();
    } else if (next_ < size_) {
      slot = next_++;
    } else {
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true)) logf("gpu: %u host views of one type in use; no more", size_);
      return {};
    }
    return {start_.ptr + SIZE_T{slot} * stride_};
  }
  void give_back(D3D12_CPU_DESCRIPTOR_HANDLE h) { free_.push_back(static_cast<UINT>((h.ptr - start_.ptr) / stride_)); }

 private:
  D3D12_DESCRIPTOR_HEAP_TYPE type_;
  UINT size_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE start_{};
  UINT stride_ = 0, next_ = 0;
  std::vector<UINT> free_;
};

struct Host {
  std::mutex lock;
  std::unordered_map<const void*, ComPtr<ID3D12Resource>> textures;  // by console resource
  std::map<std::pair<ID3D12Resource*, uint32_t>, D3D12_CPU_DESCRIPTOR_HANDLE> rtvs, dsvs;
  ViewHeap rtv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096};
  ViewHeap dsv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024};
  // Textures of resources the title released, kept until the GPU work submitted before is done.
  std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> retired;
};

Host& host() {
  static Host* const h = new Host;
  return *h;
}

}  // namespace

ID3D12Resource* host_texture(const void* console_resource, bool create) {
  present::HostGpu* gpu = present::host_gpu();
  if (!gpu || !console_resource) return nullptr;
  Host& h = host();
  std::lock_guard g(h.lock);
  if (auto it = h.textures.find(console_resource); it != h.textures.end()) return it->second.Get();
  if (!create) return nullptr;
  D3D12_RESOURCE_DESC d{};
  if (!resource_memory(console_resource, &d, nullptr)) return nullptr;
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || !(d.Flags & kGpuWritten)) return nullptr;
  // The console's own flag bits and layouts stay on the console side.
  d.Flags &= kGpuWritten | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
  if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) d.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  d.Alignment = 0;
  // Multisampled targets are single-sampled here for now: shaders sample them as plain 2D
  // textures (the console reads sample 0 that way), which D3D12 allows only without samples.
  d.SampleDesc = {1, 0};
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> t;
  const HRESULT hr =
      gpu->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
  if (t) t->SetName(widen("target " + std::to_string(d.Width) + "x" + std::to_string(d.Height)).c_str());
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("gpu: host texture %llux%u x%u, %u mips, format %u, flags 0x%x for %p%s", static_cast<unsigned long long>(d.Width),
         d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, d.Flags, console_resource,
         FAILED(hr) ? (" failed: 0x" + std::to_string(static_cast<unsigned long>(hr))).c_str() : "");
  }
  h.textures[console_resource] = t;  // a failure is remembered too: not tried every frame
  return t.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE host_rtv(ID3D12Resource* texture, uint32_t format) {
  present::HostGpu* gpu = present::host_gpu();
  if (!gpu || !texture) return {};
  Host& h = host();
  std::lock_guard g(h.lock);
  const auto key = std::make_pair(texture, format);
  if (auto it = h.rtvs.find(key); it != h.rtvs.end()) return it->second;
  const D3D12_CPU_DESCRIPTOR_HANDLE slot = h.rtv_heap.take(gpu->device.Get());
  if (!slot.ptr) return {};
  D3D12_RENDER_TARGET_VIEW_DESC v{};
  v.Format = typed_format(format ? static_cast<DXGI_FORMAT>(format) : texture->GetDesc().Format);
  v.ViewDimension = texture->GetDesc().DepthOrArraySize > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2D;
  if (v.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) v.Texture2DArray.ArraySize = texture->GetDesc().DepthOrArraySize;
  gpu->device->CreateRenderTargetView(texture, &v, slot);
  h.rtvs[key] = slot;
  return slot;
}

DXGI_FORMAT host_dsv_format(ID3D12Resource* texture, uint32_t format) {
  if (!texture) return DXGI_FORMAT_UNKNOWN;
  return depth_format(format ? static_cast<DXGI_FORMAT>(format) : texture->GetDesc().Format);
}

DXGI_FORMAT host_rtv_format(ID3D12Resource* texture, uint32_t format) {
  if (!texture) return DXGI_FORMAT_UNKNOWN;
  return typed_format(format ? static_cast<DXGI_FORMAT>(format) : texture->GetDesc().Format);
}

D3D12_CPU_DESCRIPTOR_HANDLE host_dsv(ID3D12Resource* texture, uint32_t format) {
  present::HostGpu* gpu = present::host_gpu();
  if (!gpu || !texture) return {};
  Host& h = host();
  std::lock_guard g(h.lock);
  const auto key = std::make_pair(texture, format);
  if (auto it = h.dsvs.find(key); it != h.dsvs.end()) return it->second;
  const D3D12_CPU_DESCRIPTOR_HANDLE slot = h.dsv_heap.take(gpu->device.Get());
  if (!slot.ptr) return {};
  D3D12_DEPTH_STENCIL_VIEW_DESC v{};
  v.Format = depth_format(format ? static_cast<DXGI_FORMAT>(format) : texture->GetDesc().Format);
  v.ViewDimension = texture->GetDesc().DepthOrArraySize > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2D;
  if (v.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) v.Texture2DArray.ArraySize = texture->GetDesc().DepthOrArraySize;
  gpu->device->CreateDepthStencilView(texture, &v, slot);
  h.dsvs[key] = slot;
  return slot;
}

void forget_host_texture(const void* console_resource) {
  Host& h = host();
  std::lock_guard g(h.lock);
  auto it = h.textures.find(console_resource);
  if (it == h.textures.end()) return;
  ID3D12Resource* t = it->second.Get();
  for (auto v = h.rtvs.begin(); v != h.rtvs.end();) {
    if (v->first.first == t) {
      h.rtv_heap.give_back(v->second);
      v = h.rtvs.erase(v);
    } else {
      ++v;
    }
  }
  for (auto v = h.dsvs.begin(); v != h.dsvs.end();) {
    if (v->first.first == t) {
      h.dsv_heap.give_back(v->second);
      v = h.dsvs.erase(v);
    } else {
      ++v;
    }
  }
  // Work submitted so far may still use it: it is released once that work is done.
  if (present::HostGpu* gpu = present::host_gpu(); gpu && t) {
    std::erase_if(h.retired, [&](const auto& r) { return gpu->done(r.first); });
    h.retired.emplace_back(gpu->signal(), std::move(it->second));
  }
  h.textures.erase(it);
}

}  // namespace am::rt::d3d12x
