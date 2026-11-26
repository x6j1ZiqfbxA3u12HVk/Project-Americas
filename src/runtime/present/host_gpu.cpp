#include "runtime/present/host_gpu.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <atomic>
#include <string>

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
  if (FAILED(device->GetDeviceRemovedReason())) report_device_removed(device.Get());
  return v;
}

bool dred_enabled() {
  static const bool on = GetEnvironmentVariableA("AMERICAS_D3D12_DRED", nullptr, 0) != 0;
  return on;
}

namespace {

const char* breadcrumb_name(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "marker";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    default: return nullptr;
  }
}

}  // namespace

// With AMERICAS_D3D12_DRED, the device keeps breadcrumbs (each list's operations and how far the
// GPU got) and page faults; on device loss they go to the log, once.
std::atomic<DispatchNamer> g_dispatch_namer{nullptr};
void set_dispatch_namer(DispatchNamer namer) { g_dispatch_namer = namer; }

void report_device_removed(ID3D12Device* device) {
  static std::atomic<bool> told{false};
  if (told.exchange(true)) return;
  logf("gpu: the device was lost (0x%08lx)%s", static_cast<unsigned long>(device->GetDeviceRemovedReason()),
       dred_enabled() ? "; DRED follows" : "; set AMERICAS_D3D12_DRED=1 for where");
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
  if (!dred_enabled() || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
    int lists = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n && lists < 64; n = n->pNext, ++lists) {
      const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      if (done >= n->BreadcrumbCount) continue;  // finished: not the culprit
      logf("gpu: DRED: list %p stopped after %u of %u operations", static_cast<const void*>(n->pCommandList), done,
           n->BreadcrumbCount);
      const UINT from = done > 3 ? done - 3 : 0, to = std::min<UINT>(n->BreadcrumbCount, done + 4);
      for (UINT i = from; i < to; ++i) {
        const char* name = breadcrumb_name(n->pCommandHistory[i]);
        std::string context;
        for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c) {
          if (n->pBreadcrumbContexts[c].BreadcrumbIndex == i && n->pBreadcrumbContexts[c].pContextString) {
            context = narrow(n->pBreadcrumbContexts[c].pContextString);
          }
        }
        if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DISPATCH && context.empty()) {
          // Which program: the list's dispatches are counted up to this one.
          uint32_t nth = 0;
          for (UINT j = 0; j < i; ++j) nth += n->pCommandHistory[j] == D3D12_AUTO_BREADCRUMB_OP_DISPATCH;
          if (const DispatchNamer namer = g_dispatch_namer.load()) context = namer(n->pCommandList, nth);
        }
        logf("gpu: DRED:   %s%u %s%s%s", i == done ? "-> " : "   ", i, name ? name : ("op " + std::to_string(n->pCommandHistory[i])).c_str(),
             context.empty() ? "" : ": ", context.c_str());
      }
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
    logf("gpu: DRED: page fault at GPU address 0x%s", hex(fault.PageFaultVA).c_str());
    auto list = [](const char* what, const D3D12_DRED_ALLOCATION_NODE1* n) {
      for (int k = 0; n && k < 16; n = n->pNext, ++k) {
        logf("gpu: DRED:   %s allocation: type %d, %s", what, static_cast<int>(n->AllocationType),
             n->ObjectNameW ? narrow(n->ObjectNameW).c_str() : n->ObjectNameA ? n->ObjectNameA : "(no name)");
      }
    };
    list("live", fault.pHeadExistingAllocationNode);
    list("freed", fault.pHeadRecentFreedAllocationNode);
  }
  // Every list that had not finished, and the last operations of the rest.
  int shown = 0;
  for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n && shown < 6; n = n->pNext) {
    const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
    if (done < n->BreadcrumbCount || !n->BreadcrumbCount) continue;
    ++shown;
    std::string last;
    for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c) {
      if (n->pBreadcrumbContexts[c].pContextString) last = narrow(n->pBreadcrumbContexts[c].pContextString);
    }
    logf("gpu: DRED: finished list %p (%u operations), last marker: %s", static_cast<const void*>(n->pCommandList),
         n->BreadcrumbCount, last.empty() ? "(none)" : last.c_str());
  }
}

void HostGpu::wait(uint64_t value) {
  if (done(value)) return;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  fence->SetEventOnCompletion(value, event);
  WaitForSingleObject(event, INFINITE);
  CloseHandle(event);
}

namespace {

// AMERICAS_D3D12_DEBUG: D3D12's debug layer, its warnings and errors in our log.
void CALLBACK on_debug_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
  // Errors: the first 200. Warnings: the first 5 of each id (some come every frame).
  static std::atomic<int> errors{0};
  static std::atomic<uint8_t> warnings[4096]{};
  if (severity > D3D12_MESSAGE_SEVERITY_WARNING) return;
  if (severity == D3D12_MESSAGE_SEVERITY_WARNING) {
    if (warnings[static_cast<unsigned>(id) % 4096].fetch_add(1, std::memory_order_relaxed) >= 5) return;
  } else if (errors.fetch_add(1, std::memory_order_relaxed) >= 200) {
    return;
  }
  logf("d3d12 debug (%s, id %d): %s", severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning", static_cast<int>(id), text);
}

HostGpu* make() {
  if (dred_enabled()) {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> d;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) {
      d->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      d->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      d->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      logf("gpu: DRED on (breadcrumbs, page faults)");
    }
  }
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
