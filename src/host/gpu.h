#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace am::host {

// What a host adapter offers, as far as the GPU layer cares.
struct GpuInfo {
  uint32_t index = 0;  // DXGI order by high-performance preference
  std::string name;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint64_t luid = 0;
  uint64_t dedicated_vram = 0;
  uint64_t shared_memory = 0;
  uint64_t local_budget = 0;     // what the OS lets this process use now
  uint64_t nonlocal_budget = 0;
  bool software = false;

  bool d3d12 = false;  // a device could be created
  std::string d3d12_error;
  uint32_t max_feature_level = 0;  // D3D_FEATURE_LEVEL, e.g. 0xc200 for 12_2
  uint32_t shader_model = 0;       // D3D_SHADER_MODEL, e.g. 0x68 for 6.8
  int resource_binding_tier = 0;
  int tiled_resources_tier = 0;
  int raytracing_tier = 0;   // 0, 10 (1.0), 11 (1.1)
  int mesh_shader_tier = 0;  // 0 or 10 (1.0)
  int sampler_feedback_tier = 0;
  bool wave_ops = false;
  uint32_t wave_lanes_min = 0;
  uint32_t wave_lanes_max = 0;
  bool int64_shader_ops = false;
  bool atomic64_typed = false;
  bool atomic64_groupshared = false;
  bool atomic64_heap = false;
  bool enhanced_barriers = false;
  std::optional<bool> gpu_upload_heaps;  // CPU-writable VRAM (needs Resizable BAR); nullopt: runtime too old to say
  bool uma = false;
  bool cache_coherent_uma = false;
};

std::vector<GpuInfo> enumerate_gpus();

// Picks the adapter the GPU layer will run on. Every adapter that cannot run the title's
// work (software, shader model below 6.6, no mesh shaders, resource binding below tier 3) is
// rejected with a reason; among the rest, 64-lane waves and then more VRAM win.
struct AdapterChoice {
  std::optional<size_t> chosen;     // index into the list given
  std::vector<std::string> notes;   // rejections and caveats
};
AdapterChoice select_target_adapter(std::span<const GpuInfo> gpus);

std::string feature_level_string(uint32_t level);  // "12_2"
std::string shader_model_string(uint32_t model);   // "6.8"
std::string tier_string(int tier);                 // 10 -> "1.0", 3 -> "3"

}  // namespace am::host
