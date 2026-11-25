#include "runtime/present/presenter.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include "runtime/log.h"
#include "runtime/present/host_gpu.h"

namespace am::rt::present {

using Microsoft::WRL::ComPtr;

DXGI_FORMAT swap_chain_format(uint32_t format) {
  switch (static_cast<DXGI_FORMAT>(format)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

uint32_t swap_chain_pixel_bytes(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default: return 0;
  }
}

namespace {

constexpr UINT kBuffers = 2;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// The title's memory may be bad: a fault fails the frame, not the process.
bool copy_rows(uint8_t* to, uint64_t to_pitch, const uint8_t* from, uint64_t from_pitch, uint64_t row_bytes,
               uint32_t rows) {
  __try {
    if (to_pitch == from_pitch && row_bytes == to_pitch) {
      std::memcpy(to, from, to_pitch * rows);
    } else {
      for (uint32_t y = 0; y < rows; ++y) std::memcpy(to + y * to_pitch, from + y * from_pitch, row_bytes);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

ComPtr<ID3D12Resource> make_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, uint64_t size,
                                   D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = heap_type;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) {
    return nullptr;
  }
  return r;
}

}  // namespace

struct Presenter::State {
  HWND window = nullptr;
  std::mutex lock;
  HostGpu* gpu = nullptr;
  bool failed = false;
  uint64_t frame = 0;

  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<IDXGISwapChain3> swap;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t color_space = ~0u;

  struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> upload;
    uint8_t* mapped = nullptr;
    uint64_t upload_size = 0;
    uint64_t fence_value = 0;
  } slots[kBuffers];

  ComPtr<ID3D12Resource> readback;
  uint64_t readback_size = 0;

  bool start() {
    gpu = host_gpu();
    if (!gpu) return fail("no D3D12 device");
    for (Slot& s : slots) {
      if (FAILED(gpu->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)))) {
        return fail("no command allocator");
      }
    }
    if (FAILED(gpu->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots[0].allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list)))) {
      return fail("no command list");
    }
    list->Close();
    gpu->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    return true;
  }

  bool fail(const char* why) {
    failed = true;
    logf("present: %s; frames are counted but not shown", why);
    return false;
  }

  bool ensure_swap_chain(DXGI_FORMAT f, uint32_t w, uint32_t h) {
    if (swap && f == format && w == width && h == height) return true;
    gpu->wait_idle();
    swap.Reset();
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = w;
    d.Height = h;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kBuffers;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> s1;
    const HRESULT hr = gpu->factory->CreateSwapChainForHwnd(gpu->queue.Get(), window, &d, nullptr, nullptr, &s1);
    if (FAILED(hr) || FAILED(s1.As(&swap))) {
      logf("present: no swap chain for %ux%u, format %u (0x%08lx)", w, h, f, static_cast<unsigned long>(hr));
      return false;
    }
    format = f;
    width = w;
    height = h;
    color_space = ~0u;
    logf("present: swap chain %ux%u, format %u", w, h, f);
    return true;
  }

  bool ensure_upload(Slot& s, uint64_t size) {
    if (s.upload && s.upload_size >= size) return true;
    s.upload = make_buffer(gpu->device.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    s.mapped = nullptr;
    s.upload_size = 0;
    void* p = nullptr;
    if (!s.upload || FAILED(s.upload->Map(0, nullptr, &p))) return false;
    s.mapped = static_cast<uint8_t*>(p);
    s.upload_size = size;
    return true;
  }

  void set_color_space(uint32_t space) {
    if (space == color_space) return;
    color_space = space;
    UINT support = 0;
    const auto cs = static_cast<DXGI_COLOR_SPACE_TYPE>(space);
    if (SUCCEEDED(swap->CheckColorSpaceSupport(cs, &support)) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
      swap->SetColorSpace1(cs);
      logf("present: color space %u", space);
    } else {
      logf("present: color space %u not supported by this display; shown as is", space);
    }
  }
};

Presenter::Presenter(HWND window) : s_(new State) { s_->window = window; }

Presenter::~Presenter() {
  {
    std::lock_guard g(s_->lock);
    if (s_->gpu) s_->gpu->wait_idle();
  }
  delete s_;
}

bool Presenter::usable() const { return !s_->failed && host_gpu() != nullptr; }

bool Presenter::present(const Image& image, std::vector<uint8_t>* capture) {
  State& s = *s_;
  std::lock_guard g(s.lock);
  if (s.failed || !s.window) return false;
  if (!s.gpu && !s.start()) return false;
  const DXGI_FORMAT f = swap_chain_format(image.format);
  const uint32_t pixel = swap_chain_pixel_bytes(f);
  if (!pixel || (!image.gpu && !image.memory) || !image.width || !image.height) return false;
  if (!s.ensure_swap_chain(f, image.width, image.height)) return false;

  const uint64_t row_bytes = uint64_t{image.width} * pixel;
  const uint64_t pitch = align_up(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  State::Slot& slot = s.slots[s.frame % kBuffers];
  s.gpu->wait(slot.fence_value);

  slot.allocator->Reset();
  s.list->Reset(slot.allocator.Get(), nullptr);
  ComPtr<ID3D12Resource> target;
  s.swap->GetBuffer(s.swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target));
  D3D12_RESOURCE_BARRIER b[2] = {transition(target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
  UINT barriers = 1;
  if (image.gpu) b[barriers++] = transition(image.gpu, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
  s.list->ResourceBarrier(barriers, b);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = target.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION footprint{};
  footprint.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  footprint.PlacedFootprint.Footprint = {f, image.width, image.height, 1, static_cast<UINT>(pitch)};
  if (image.gpu) {
    // GPU to GPU: the region of the title's render target.
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = image.gpu;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{image.left, image.top, 0, image.left + image.width, image.top + image.height, 1};
    s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  } else {
    if (!s.ensure_upload(slot, pitch * image.height) ||
        !copy_rows(slot.mapped, pitch, image.memory, image.row_pitch, std::min<uint64_t>(row_bytes, image.row_pitch),
                   image.height)) {
      s.list->Close();
      return false;
    }
    footprint.pResource = slot.upload.Get();
    s.list->CopyTextureRegion(&dst, 0, 0, 0, &footprint, nullptr);
  }
  barriers = 0;
  if (capture) {
    if (!s.readback || s.readback_size < pitch * image.height) {
      s.readback = make_buffer(s.gpu->device.Get(), D3D12_HEAP_TYPE_READBACK, pitch * image.height,
                               D3D12_RESOURCE_STATE_COPY_DEST);
      s.readback_size = s.readback ? pitch * image.height : 0;
    }
    const D3D12_RESOURCE_BARRIER to_source = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    s.list->ResourceBarrier(1, &to_source);
    if (s.readback) {
      D3D12_TEXTURE_COPY_LOCATION back = footprint;
      back.pResource = s.readback.Get();
      s.list->CopyTextureRegion(&back, 0, 0, 0, &dst, nullptr);
    }
    b[barriers++] = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  } else {
    b[barriers++] = transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  }
  if (image.gpu) b[barriers++] = transition(image.gpu, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  s.list->ResourceBarrier(barriers, b);
  if (FAILED(s.list->Close())) return false;
  HRESULT hr;
  {
    std::lock_guard q(s.gpu->submit_lock);
    ID3D12CommandList* lists[] = {s.list.Get()};
    s.gpu->queue->ExecuteCommandLists(1, lists);
    s.set_color_space(image.color_space);
    hr = s.swap->Present(1, 0);
  }
  slot.fence_value = s.gpu->signal();
  ++s.frame;
  if (FAILED(hr)) {
    logf("present: Present failed (0x%08lx)", static_cast<unsigned long>(hr));
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) s.fail("the device was lost");
    return false;
  }
  if (capture && s.readback) {
    s.gpu->wait(slot.fence_value);
    void* p = nullptr;
    if (FAILED(s.readback->Map(0, nullptr, &p))) return false;
    capture->resize(row_bytes * image.height);
    for (uint32_t y = 0; y < image.height; ++y) {
      std::memcpy(capture->data() + y * row_bytes, static_cast<const uint8_t*>(p) + y * pitch, row_bytes);
    }
    D3D12_RANGE none{0, 0};
    s.readback->Unmap(0, &none);
  }
  return true;
}

}  // namespace am::rt::present
