#pragma once

#include <string>

// The host's D3D12 device, one per process: the graphics back end records into it and the
// presenter shows through it. Made on first use on the high-performance adapter; nullptr when
// the PC has no D3D12 GPU (frames are then counted but not drawn).
//
// Console commands go to it as they are made (each console call becomes the host call that does
// the same), and shaders arrive compiled ahead of time: there is no translation while the game
// runs.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>

namespace am::rt::present {

struct HostGpu {
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;  // direct: the console's queues all submit here
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;

  // Signals the fence after the work submitted so far; returns the value.
  uint64_t signal();
  // Submits `lists` and signals after them, in one step; returns the value.
  uint64_t submit(UINT count, ID3D12CommandList* const* lists);
  bool done(uint64_t value) const { return fence->GetCompletedValue() >= value; }
  void wait(uint64_t value);
  void wait_idle() { wait(signal()); }

  std::mutex submit_lock;  // queue submissions and their fence values stay in order

 private:
  uint64_t next_ = 1;
};

HostGpu* host_gpu();

// AMERICAS_D3D12_DRED: D3D12's device-removed data (breadcrumbs, page faults) is kept, and
// written to the log when the device is lost.
bool dred_enabled();
void report_device_removed(ID3D12Device* device);
// Names the `nth` dispatch recorded in a host command list (set by the D3D12.X layer), for the
// device-lost report.
using DispatchNamer = std::string (*)(const void* list, uint32_t nth);
void set_dispatch_namer(DispatchNamer namer);

}  // namespace am::rt::present
