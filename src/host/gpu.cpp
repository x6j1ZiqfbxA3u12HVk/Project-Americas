#include "host/gpu.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>

#include "core/file_view.h"
#include "core/text.h"

using Microsoft::WRL::ComPtr;

namespace am::host {
namespace {

std::string hresult_text(HRESULT hr) {
  char code[16];
  std::snprintf(code, sizeof code, "0x%08lx", static_cast<unsigned long>(hr));
  return std::string(code) + ": " + win32_error_text(static_cast<unsigned long>(hr));
}

template <class T>
bool query(ID3D12Device* device, D3D12_FEATURE feature, T& data) {
  return SUCCEEDED(device->CheckFeatureSupport(feature, &data, sizeof(T)));
}

void probe_d3d12(IDXGIAdapter1* adapter, GpuInfo& g) {
  ComPtr<ID3D12Device> device;
  const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr)) {
    g.d3d12_error = hresult_text(hr);
    return;
  }
  g.d3d12 = true;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
  fl.NumFeatureLevels = static_cast<UINT>(std::size(levels));
  fl.pFeatureLevelsRequested = levels;
  if (query(device.Get(), D3D12_FEATURE_FEATURE_LEVELS, fl)) g.max_feature_level = fl.MaxSupportedFeatureLevel;

  // Ask for the newest model first; a runtime that does not know a model rejects the query.
  const D3D_SHADER_MODEL models[] = {D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7,
                                     D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
                                     D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1,
                                     D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1};
  for (const D3D_SHADER_MODEL model : models) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm{model};
    if (query(device.Get(), D3D12_FEATURE_SHADER_MODEL, sm)) {
      g.shader_model = sm.HighestShaderModel;
      break;
    }
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS, o)) {
    g.resource_binding_tier = o.ResourceBindingTier;
    g.tiled_resources_tier = o.TiledResourcesTier;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS1, o1)) {
    g.wave_ops = o1.WaveOps;
    g.wave_lanes_min = o1.WaveLaneCountMin;
    g.wave_lanes_max = o1.WaveLaneCountMax;
    g.int64_shader_ops = o1.Int64ShaderOps;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS5, o5)) g.raytracing_tier = o5.RaytracingTier;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS7, o7)) {
    g.mesh_shader_tier = o7.MeshShaderTier;
    g.sampler_feedback_tier = o7.SamplerFeedbackTier;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS9, o9)) {
    g.atomic64_typed = o9.AtomicInt64OnTypedResourceSupported;
    g.atomic64_groupshared = o9.AtomicInt64OnGroupSharedSupported;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS11, o11)) g.atomic64_heap = o11.AtomicInt64OnDescriptorHeapResourceSupported;
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS12, o12)) g.enhanced_barriers = o12.EnhancedBarriersSupported;
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
  if (query(device.Get(), D3D12_FEATURE_D3D12_OPTIONS16, o16)) g.gpu_upload_heaps = o16.GPUUploadHeapSupported != FALSE;
  D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
  if (query(device.Get(), D3D12_FEATURE_ARCHITECTURE1, arch)) {
    g.uma = arch.UMA;
    g.cache_coherent_uma = arch.CacheCoherentUMA;
  }
}

}  // namespace

std::vector<GpuInfo> enumerate_gpus() {
  std::vector<GpuInfo> gpus;
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return gpus;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    GpuInfo g;
    g.index = i;
    g.name = narrow(desc.Description);
    g.vendor_id = desc.VendorId;
    g.device_id = desc.DeviceId;
    g.luid = (uint64_t{static_cast<uint32_t>(desc.AdapterLuid.HighPart)} << 32) | desc.AdapterLuid.LowPart;
    g.dedicated_vram = desc.DedicatedVideoMemory;
    g.shared_memory = desc.SharedSystemMemory;
    g.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(adapter.As(&adapter3))) {
      DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) g.local_budget = local.Budget;
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        g.nonlocal_budget = nonlocal.Budget;
      }
    }
    probe_d3d12(adapter.Get(), g);
    gpus.push_back(std::move(g));
  }
  return gpus;
}

AdapterChoice select_target_adapter(std::span<const GpuInfo> gpus) {
  AdapterChoice choice;
  uint64_t best_score = 0;
  for (size_t i = 0; i < gpus.size(); ++i) {
    const GpuInfo& g = gpus[i];
    std::string why;
    auto reject = [&](const char* reason) { why += why.empty() ? reason : std::string("; ") + reason; };
    if (g.software) reject("software adapter");
    if (!g.d3d12) {
      reject("no Direct3D 12 device");
    } else {
      if (g.shader_model < D3D_SHADER_MODEL_6_6) reject("shader model below 6.6");
      if (g.mesh_shader_tier < D3D12_MESH_SHADER_TIER_1) reject("no mesh shaders");
      if (g.resource_binding_tier < D3D12_RESOURCE_BINDING_TIER_3) reject("resource binding below tier 3");
    }
    if (!why.empty()) {
      choice.notes.push_back(g.name + ": not usable (" + why + ")");
      continue;
    }
    const uint64_t score = (g.wave_lanes_max >= 64 ? uint64_t{1} << 62 : 0) + g.dedicated_vram;
    if (!choice.chosen || score > best_score) {
      choice.chosen = i;
      best_score = score;
    }
  }
  if (choice.chosen) {
    const GpuInfo& g = gpus[*choice.chosen];
    if (g.wave_lanes_max < 64) {
      choice.notes.push_back(g.name + ": no 64-lane waves; console wave64 programs need emulation (slower)");
    }
    if (g.gpu_upload_heaps.value_or(false) == false) {
      choice.notes.push_back(g.name + ": no CPU-writable VRAM (Resizable BAR off or unsupported); CPU-written GPU data is uploaded");
    }
  }
  return choice;
}

std::string feature_level_string(uint32_t level) {
  if (!level) return "none";
  return std::to_string(level >> 12) + "_" + std::to_string((level >> 8) & 0xf);
}

std::string shader_model_string(uint32_t model) {
  if (!model) return "none";
  return std::to_string(model >> 4) + "." + std::to_string(model & 0xf);
}

std::string tier_string(int tier) {
  // D3D12 encodes "major.minor" tiers as 10 * major + minor, except sampler feedback (90 = 0.9, 100 = 1.0).
  if (tier >= 90) return std::to_string(tier / 100) + "." + std::to_string((tier % 100) / 10);
  if (tier >= 10) return std::to_string(tier / 10) + "." + std::to_string(tier % 10);
  return std::to_string(tier);
}

}  // namespace am::host
