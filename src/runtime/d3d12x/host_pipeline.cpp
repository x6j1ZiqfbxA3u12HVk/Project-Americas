// Host pipelines from the console's: shaders compiled ahead of time (am-shaders precompile,
// <cache>\shaders\dxil\rt\<marker>.dxil, looked up by the 24-byte marker each console program
// starts with), state from the title's description, and the root signature every translated
// graphics program shares.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "runtime/d3d12x/gpu_memory.h"
#include "runtime/d3d12x/host.h"
#include "runtime/log.h"
#include "runtime/present/host_gpu.h"
#include "runtime/process.h"
#include "shader/console_program.h"
#include "shader/container.h"

namespace am::rt::d3d12x {
namespace {

using Microsoft::WRL::ComPtr;

std::string hex_of(std::span<const uint8_t> b) {
  static const char* const kHex = "0123456789abcdef";
  std::string h;
  for (uint8_t x : b) {
    h += kHex[x >> 4];
    h += kHex[x & 15];
  }
  return h;
}

shader::HwStage hw_stage(uint32_t subobject) {
  switch (subobject) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return shader::HwStage::Vs;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return shader::HwStage::Ps;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return shader::HwStage::Cs;
    default: return shader::HwStage::Ls;  // none we precompile
  }
}

}  // namespace

std::string program_marker(std::span<const uint8_t> container, uint32_t subobject) {
  std::string error;
  const auto c = shader::parse_container(container, error);
  const shader::Part* dxil = c ? c->find("DXIL") : nullptr;
  if (!dxil) return {};
  const auto s = shader::read_console_shader(dxil->data, error);
  if (!s) return {};
  const shader::HwStage want = hw_stage(subobject);
  for (const shader::NativeBlob& b : s->blobs) {
    for (const shader::NativeProgram& p : b.programs) {
      if (p.stage == want && p.code_size >= 24) return hex_of(b.code(p).subspan(0, 24));
    }
  }
  return {};
}

bool load_precompiled(const std::string& marker, std::vector<uint8_t>& dxil) {
  TitleProcess* p = process();
  if (marker.empty() || !p || p->cache_dir().empty()) return false;
  const std::filesystem::path file = p->cache_dir() / "shaders" / "dxil" / "rt" / (marker + ".dxil");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    static std::mutex lock;
    static std::set<std::string> told;
    std::lock_guard g(lock);
    if (told.insert(marker).second && told.size() <= 64) {
      logf("gpu: no precompiled shader %s (run am-shaders precompile)", marker.c_str());
    }
    return false;
  }
  dxil.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !dxil.empty();
}

ID3D12PipelineState* make_host_graphics_pipeline(D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, const char* what) {
  present::HostGpu* gpu = present::host_gpu();
  ID3D12RootSignature* root = translated_graphics_root_signature();
  if (!gpu || !root) return nullptr;
  desc.pRootSignature = root;
  ID3D12PipelineState* pso = nullptr;
  const HRESULT hr = gpu->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("gpu: host pipeline for %s: %s (0x%08lx)", what, SUCCEEDED(hr) ? "made" : "refused", static_cast<unsigned long>(hr));
  }
  return SUCCEEDED(hr) ? pso : nullptr;
}

}  // namespace am::rt::d3d12x
