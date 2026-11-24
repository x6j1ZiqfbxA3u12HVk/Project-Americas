// am-probe: reports what this PC offers the project (CPU instruction sets, memory, GPUs, the
// shader compiler) and which GPU the runtime would pick.
//
//   am-probe [--json <file>]

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/text.h"
#include "host/cpu.h"
#include "host/dxc.h"
#include "host/gpu.h"
#include "host/system.h"

namespace {

using namespace am;
using namespace am::host;

const char* yes_no(bool b) { return b ? "yes" : "no"; }

std::string vendor_name(uint32_t id) {
  switch (id) {
    case 0x1002: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    default: return "0x" + hex(id, 4);
  }
}

int usage() {
  std::fprintf(stderr, "usage: am-probe [--json <file>]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring json_path;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else {
      return usage();
    }
  }

  const OsVersion os = os_version();
  const CpuidSnapshot cpu = read_cpuid();
  const Topology topo = processor_topology();
  const MemoryInfo mem = memory_info();
  const std::vector<GpuInfo> gpus = enumerate_gpus();
  const AdapterChoice choice = select_target_adapter(gpus);
  const DxcInfo dxc = probe_dxc();
  const std::vector<CpuFeature> missing = missing_console_features(cpu);

  std::printf("Project Americas host probe\n\n");
  std::printf("Windows   %u.%u.%u.%u\n", os.major, os.minor, os.build, os.revision);
  std::printf("CPU       %s (%s)\n", cpu.brand.c_str(), cpu.vendor.c_str());
  std::printf("          %u cores, %u logical processors, L3 %s\n", topo.cores, topo.logical,
              format_bytes(topo.l3_bytes).c_str());
  if (missing.empty()) {
    std::printf("          has every console (Zen 2) instruction set\n");
  } else {
    std::printf("          console (Zen 2) instruction sets missing here (title code using them gets patched):\n");
    for (const CpuFeature f : missing) {
      const CpuFeatureInfo& info = feature_info(f);
      std::printf("            %-16s %s\n", info.name, info.note);
    }
  }
  std::printf("Memory    %s physical (%s free), commit limit %s (%s in use)\n", format_bytes(mem.total_physical).c_str(),
              format_bytes(mem.available_physical).c_str(), format_bytes(mem.commit_limit).c_str(),
              format_bytes(mem.commit_total).c_str());
  std::printf("          page files about %s; large pages %s, \"Lock pages in memory\" privilege: %s\n",
              format_bytes(mem.commit_limit > mem.total_physical ? mem.commit_limit - mem.total_physical : 0).c_str(),
              mem.large_page_minimum ? format_bytes(mem.large_page_minimum).c_str() : "unsupported",
              yes_no(mem.lock_pages_privilege));

  std::printf("\nGPUs\n");
  for (size_t i = 0; i < gpus.size(); ++i) {
    const GpuInfo& g = gpus[i];
    std::printf("  [%zu] %s (%s %04x)%s\n", i, g.name.c_str(), vendor_name(g.vendor_id).c_str(), g.device_id,
                g.software ? " [software]" : "");
    std::printf("      VRAM %s, shared %s, budget now %s local / %s system\n", format_bytes(g.dedicated_vram).c_str(),
                format_bytes(g.shared_memory).c_str(), format_bytes(g.local_budget).c_str(),
                format_bytes(g.nonlocal_budget).c_str());
    if (!g.d3d12) {
      std::printf("      Direct3D 12: %s\n", g.d3d12_error.c_str());
      continue;
    }
    std::printf("      D3D12 feature level %s, shader model %s, resource binding tier %d, tiled resources tier %d\n",
                feature_level_string(g.max_feature_level).c_str(), shader_model_string(g.shader_model).c_str(),
                g.resource_binding_tier, g.tiled_resources_tier);
    std::printf("      waves %u-%u lanes, int64 %s, 64-bit atomics typed/heap/groupshared %s/%s/%s\n", g.wave_lanes_min,
                g.wave_lanes_max, yes_no(g.int64_shader_ops), yes_no(g.atomic64_typed), yes_no(g.atomic64_heap),
                yes_no(g.atomic64_groupshared));
    std::printf("      mesh shaders %s, ray tracing %s, sampler feedback %s, enhanced barriers %s\n",
                g.mesh_shader_tier ? tier_string(g.mesh_shader_tier).c_str() : "no",
                g.raytracing_tier ? tier_string(g.raytracing_tier).c_str() : "no",
                g.sampler_feedback_tier ? tier_string(g.sampler_feedback_tier).c_str() : "no", yes_no(g.enhanced_barriers));
    std::printf("      CPU-writable VRAM (GPU upload heaps, needs Resizable BAR): %s; UMA %s\n",
                g.gpu_upload_heaps ? yes_no(*g.gpu_upload_heaps) : "unknown (D3D12 runtime too old to say)",
                yes_no(g.uma));
  }
  if (choice.chosen) {
    std::printf("\nTarget GPU: [%zu] %s\n", *choice.chosen, gpus[*choice.chosen].name.c_str());
  } else {
    std::printf("\nTarget GPU: none of the adapters can run the title's GPU work\n");
  }
  for (const std::string& note : choice.notes) std::printf("  - %s\n", note.c_str());

  std::printf("\nShader compiler\n");
  if (dxc.found) {
    std::printf("  DXC %s from %s (%s)%s\n", dxc.version.c_str(), narrow(dxc.dir.wstring()).c_str(), dxc.source.c_str(),
                dxc.separate_validator ? ", dxil.dll present" : "");
  } else {
    std::printf("  not usable: %s\n", dxc.error.c_str());
  }

  if (!json_path.empty()) {
    JsonWriter j;
    j.begin_object();
    j.key("os").begin_object().field("major", os.major).field("minor", os.minor).field("build", os.build)
        .field("revision", os.revision).end_object();
    j.key("cpu").begin_object().field("vendor", cpu.vendor).field("brand", cpu.brand).field("cores", topo.cores)
        .field("logical", topo.logical).field("l3_bytes", topo.l3_bytes);
    j.key("missing_console_features").begin_array();
    for (const CpuFeature f : missing) j.value(feature_info(f).name);
    j.end_array().end_object();
    j.key("memory").begin_object().field("total_physical", mem.total_physical)
        .field("available_physical", mem.available_physical).field("commit_limit", mem.commit_limit)
        .field("commit_total", mem.commit_total).field("large_page_minimum", mem.large_page_minimum)
        .field("lock_pages_privilege", mem.lock_pages_privilege).end_object();
    j.key("gpus").begin_array();
    for (const GpuInfo& g : gpus) {
      j.begin_object().field("name", g.name).field("vendor_id", g.vendor_id).field("device_id", g.device_id)
          .field("dedicated_vram", g.dedicated_vram).field("shared_memory", g.shared_memory)
          .field("local_budget", g.local_budget).field("software", g.software).field("d3d12", g.d3d12);
      if (g.d3d12) {
        j.field("feature_level", feature_level_string(g.max_feature_level))
            .field("shader_model", shader_model_string(g.shader_model))
            .field("resource_binding_tier", g.resource_binding_tier).field("wave_lanes_min", g.wave_lanes_min)
            .field("wave_lanes_max", g.wave_lanes_max).field("mesh_shader_tier", g.mesh_shader_tier)
            .field("raytracing_tier", g.raytracing_tier).field("atomic64_typed", g.atomic64_typed)
            .field("atomic64_heap", g.atomic64_heap).field("enhanced_barriers", g.enhanced_barriers);
        j.key("gpu_upload_heaps");
        if (g.gpu_upload_heaps) j.value(*g.gpu_upload_heaps); else j.null();
      } else {
        j.field("d3d12_error", g.d3d12_error);
      }
      j.end_object();
    }
    j.end_array();
    j.key("target_gpu");
    if (choice.chosen) j.value(gpus[*choice.chosen].name); else j.null();
    j.key("target_notes").begin_array();
    for (const std::string& note : choice.notes) j.value(note);
    j.end_array();
    j.key("dxc").begin_object().field("found", dxc.found).field("dir", narrow(dxc.dir.wstring()))
        .field("version", dxc.version).field("error", dxc.error).end_object();
    j.end_object();

    std::ofstream out(json_path, std::ios::binary);
    out << j.str() << '\n';
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", narrow(json_path).c_str());
      return 1;
    }
    std::printf("\nwrote %s\n", narrow(json_path).c_str());
  }
  return choice.chosen ? 0 : 1;
}
