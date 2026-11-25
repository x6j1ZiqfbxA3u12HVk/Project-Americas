#include "runtime/present/host_gpu.h"

#include <d3d12sdklayers.h>

#include <atomic>

#include "core/text.h"
#include "runtime/log.h"

namespace am::rt::present {

using Microsoft::WRL::ComPtr;

uint64_t HostGpu::signal() {
  std::lock_guard g(submit_lock);
  const uint64_t v = next_++;
  queue->Signal(fence.Get(), v);
  return v;
}

uint64_t HostGpu::submit(UINT count, ID3D12CommandList* const* lists) {
  std::lock_guard g(submit_lock);
  if (count) queue->ExecuteCommandLists(count, lists);
  const uint64_t v = next_++;
  queue->Signal(fence.Get(), v);
  return v;
}

void HostGpu::wait(uint64_t value) {
  if (done(value)) return;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  fence->SetEventOnCompletion(value, event);
  WaitForSingleObject(event, INFINITE);
  CloseHandle(event);
}

namespace {

// AMERICAS_D3D12_DEBUG: D3D12's debug layer, its warnings and errors in our log (the first 200).
void CALLBACK on_debug_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
  static std::atomic<int> told{0};
  if (severity > D3D12_MESSAGE_SEVERITY_WARNING || told.fetch_add(1, std::memory_order_relaxed) >= 200) return;
  logf("d3d12 debug (%s, id %d): %s", severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning", static_cast<int>(id), text);
}

HostGpu* make() {
  const bool debug = GetEnvironmentVariableA("AMERICAS_D3D12_DEBUG", nullptr, 0) != 0;
  if (debug) {
    ComPtr<ID3D12Debug> d;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) d->EnableDebugLayer();
    else logf("gpu: no D3D12 debug layer (install Windows' Graphics Tools)");
  }
  auto* g = new HostGpu;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g->factory)))) {
    logf("gpu: no DXGI factory");
    return nullptr;
  }
  ComPtr<IDXGIFactory6> f6;
  if (SUCCEEDED(g->factory.As(&f6))) {
    f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&g->adapter));
  }
  if (!g->adapter) g->factory->EnumAdapters1(0, &g->adapter);
  if (FAILED(D3D12CreateDevice(g->adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g->device)))) {
    logf("gpu: no D3D12 device; frames are counted but not drawn");
    return nullptr;
  }
  if (debug) {
    ComPtr<ID3D12InfoQueue1> q;
    DWORD cookie = 0;
    if (SUCCEEDED(g->device.As(&q)) &&
        SUCCEEDED(q->RegisterMessageCallback(&on_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie))) {
      logf("gpu: D3D12 debug layer on; its messages follow in this log");
    }
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(g->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g->queue))) ||
      FAILED(g->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->fence)))) {
    logf("gpu: no command queue or fence");
    return nullptr;
  }
  DXGI_ADAPTER_DESC1 ad{};
  g->adapter->GetDesc1(&ad);
  logf("gpu: D3D12 on %s", narrow(ad.Description).c_str());
  return g;
}

}  // namespace

HostGpu* host_gpu() {
  static HostGpu* const g = make();
  return g;
}

}  // namespace am::rt::present
