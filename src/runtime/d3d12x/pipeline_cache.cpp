// Host pipelines kept between runs: a D3D12 pipeline library under <cache>\d3d12. The first run
// pays for the driver's compile of every pipeline (seconds each for the largest, and gigabytes
// of commit while it compiles); later runs load them. Each pipeline is named by a SHA-256 of
// what its description holds; the driver refuses a library from another driver or adapter (an
// empty one is used then) and an entry whose description differs. Library calls are serialised:
// D3D12 does not promise that stores, loads and serialising may overlap. Driver compiles run at
// most two at a time, and slow or memory-hungry ones are logged with the commit they added.

#include "runtime/d3d12x/pipeline_cache.h"

#include <psapi.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <semaphore>
#include <span>
#include <string>
#include <vector>

#include "core/sha256.h"
#include "core/text.h"
#include "runtime/log.h"
#include "runtime/present/host_gpu.h"
#include "runtime/process.h"
#include "shader/translate.h"

namespace am::rt::d3d12x {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

struct Cache {
  std::mutex lock;
  bool tried = false;
  ComPtr<ID3D12PipelineLibrary> library;
  std::vector<uint8_t> blob;  // the library was made from it: kept for the library's life
  fs::path file;
  uint64_t stored_since_save = 0;
  std::atomic<uint64_t> loaded{0}, made{0};
};

Cache& cache() {
  static Cache* const c = new Cache;
  return *c;
}

// Under the cache's lock: the library, opened from its file once (or empty).
ID3D12PipelineLibrary* library() {
  Cache& c = cache();
  if (c.tried) return c.library.Get();
  c.tried = true;
  present::HostGpu* gpu = present::host_gpu();
  TitleProcess* p = process();
  if (!gpu || !p || p->cache_dir().empty()) return nullptr;
  ComPtr<ID3D12Device1> device;
  if (FAILED(gpu->device.As(&device))) return nullptr;
  std::error_code ec;
  const fs::path dir = p->cache_dir() / L"d3d12";
  fs::create_directories(dir, ec);
  c.file = dir / L"host-pipelines.bin";
  {
    std::ifstream in(c.file, std::ios::binary);
    if (in) c.blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  HRESULT hr = c.blob.empty() ? E_FAIL : device->CreatePipelineLibrary(c.blob.data(), c.blob.size(), IID_PPV_ARGS(&c.library));
  if (FAILED(hr)) {
    if (!c.blob.empty()) logf("gpu: the pipeline library on disk is not usable here (0x%08lx): starting an empty one", static_cast<unsigned long>(hr));
    c.blob.clear();
    hr = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&c.library));
    if (FAILED(hr)) {
      logf("gpu: no pipeline library (0x%08lx): pipelines are compiled every run", static_cast<unsigned long>(hr));
      return nullptr;
    }
  } else {
    logf("gpu: pipeline library: %s from %s", format_bytes(c.blob.size()).c_str(), narrow(c.file.wstring()).c_str());
  }
  return c.library.Get();
}

// Under the cache's lock: the library written to its file (atomically).
void save_locked() {
  Cache& c = cache();
  if (!c.library || !c.stored_since_save) return;
  const SIZE_T size = c.library->GetSerializedSize();
  std::vector<uint8_t> bytes(size);
  if (!size || FAILED(c.library->Serialize(bytes.data(), size))) return;
  const fs::path tmp = c.file.wstring() + L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return;
  }
  std::error_code ec;
  fs::rename(tmp, c.file, ec);
  c.stored_since_save = 0;
}

std::wstring name_of(const char* kind, const std::vector<std::span<const uint8_t>>& parts) {
  std::vector<uint8_t> all(kind, kind + std::strlen(kind) + 1);
  const std::string version = "translator " + std::to_string(shader::kTranslatorVersion) + " root 1";
  all.insert(all.end(), version.begin(), version.end() + 1);
  for (const auto& p : parts) {
    const uint64_t n = p.size();
    all.insert(all.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 8);
    all.insert(all.end(), p.begin(), p.end());
  }
  const auto h = sha256(all);
  return h ? widen(hex_bytes(h->data(), h->size())) : std::wstring();
}

template <typename T>
std::span<const uint8_t> bytes_of(const T& x) {
  return {reinterpret_cast<const uint8_t*>(&x), sizeof x};
}
std::span<const uint8_t> code_of(const D3D12_SHADER_BYTECODE& b) {
  return {static_cast<const uint8_t*>(b.pShaderBytecode), b.pShaderBytecode ? b.BytecodeLength : 0};
}

// Driver compiles at most two at a time (each can take gigabytes while it runs).
std::counting_semaphore<2>& compiles() {
  static std::counting_semaphore<2> s(2);
  return s;
}

uint64_t commit_now() {
  PROCESS_MEMORY_COUNTERS_EX pm{};
  pm.cb = sizeof pm;
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
  return pm.PrivateUsage;
}

template <typename Make>
ID3D12PipelineState* compile(const char* what, uint64_t code_bytes, Make make) {
  compiles().acquire();
  const uint64_t before = commit_now();
  const ULONGLONG start = GetTickCount64();
  ID3D12PipelineState* pso = make();
  const ULONGLONG took = GetTickCount64() - start;
  const int64_t added = static_cast<int64_t>(commit_now()) - static_cast<int64_t>(before);
  compiles().release();
  static std::atomic<int> told{0};
  if ((took > 2000 || added > (int64_t{256} << 20)) && told.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("gpu: the driver compiled a %s pipeline (%s of DXIL) in %llu ms; process commit %s%s since", what,
         format_bytes(code_bytes).c_str(), static_cast<unsigned long long>(took), added < 0 ? "-" : "+",
         format_bytes(static_cast<uint64_t>(added < 0 ? -added : added)).c_str());
  }
  return pso;
}

void store(const std::wstring& name, ID3D12PipelineState* pso) {
  if (name.empty() || !pso) return;
  Cache& c = cache();
  std::lock_guard g(c.lock);
  ID3D12PipelineLibrary* lib = library();
  if (!lib) return;
  if (SUCCEEDED(lib->StorePipeline(name.c_str(), pso))) {
    // Saved every 64 new pipelines (and by save_pipeline_cache), so a run that ends early
    // still keeps most of what it compiled.
    if (++c.stored_since_save >= 64) save_locked();
  }
}

}  // namespace

ID3D12PipelineState* cached_compute_pipeline(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
  present::HostGpu* gpu = present::host_gpu();
  if (!gpu) return nullptr;
  const std::wstring name = name_of("compute", {code_of(desc.CS), bytes_of(desc.NodeMask), bytes_of(desc.Flags)});
  Cache& c = cache();
  if (!name.empty()) {
    std::lock_guard g(c.lock);
    ID3D12PipelineState* pso = nullptr;
    if (ID3D12PipelineLibrary* lib = library(); lib && SUCCEEDED(lib->LoadComputePipeline(name.c_str(), &desc, IID_PPV_ARGS(&pso)))) {
      ++c.loaded;
      return pso;
    }
  }
  ID3D12PipelineState* pso = compile("compute", desc.CS.BytecodeLength, [&] {
    ID3D12PipelineState* p = nullptr;
    return SUCCEEDED(gpu->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p))) ? p : nullptr;
  });
  if (pso) {
    ++c.made;
    store(name, pso);
  }
  return pso;
}

ID3D12PipelineState* cached_graphics_pipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, HRESULT* result) {
  present::HostGpu* gpu = present::host_gpu();
  if (result) *result = E_FAIL;
  if (!gpu) return nullptr;
  // Everything the description holds but pointers: their contents are added apart.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC flat = desc;
  flat.pRootSignature = nullptr;
  flat.VS = flat.PS = flat.DS = flat.HS = flat.GS = {};
  flat.StreamOutput = {};
  flat.InputLayout = {};
  flat.CachedPSO = {};
  std::string layout;
  for (UINT k = 0; k < desc.InputLayout.NumElements; ++k) {
    const D3D12_INPUT_ELEMENT_DESC& e = desc.InputLayout.pInputElementDescs[k];
    layout += (e.SemanticName ? e.SemanticName : "") + std::string("/") + std::to_string(e.SemanticIndex) + "/" + std::to_string(e.Format) +
              "/" + std::to_string(e.InputSlot) + "/" + std::to_string(e.AlignedByteOffset) + "/" + std::to_string(e.InputSlotClass) + "/" +
              std::to_string(e.InstanceDataStepRate) + ";";
  }
  const std::wstring name =
      name_of("graphics", {bytes_of(flat), code_of(desc.VS), code_of(desc.PS), code_of(desc.GS), code_of(desc.DS), code_of(desc.HS),
                           {reinterpret_cast<const uint8_t*>(layout.data()), layout.size()}});
  Cache& c = cache();
  if (!name.empty()) {
    std::lock_guard g(c.lock);
    ID3D12PipelineState* pso = nullptr;
    if (ID3D12PipelineLibrary* lib = library(); lib && SUCCEEDED(lib->LoadGraphicsPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pso)))) {
      ++c.loaded;
      if (result) *result = S_OK;
      return pso;
    }
  }
  HRESULT hr = E_FAIL;
  ID3D12PipelineState* pso = compile("graphics", desc.VS.BytecodeLength + desc.PS.BytecodeLength + desc.GS.BytecodeLength, [&] {
    ID3D12PipelineState* p = nullptr;
    hr = gpu->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&p));
    return SUCCEEDED(hr) ? p : nullptr;
  });
  if (result) *result = hr;
  if (pso) {
    ++c.made;
    store(name, pso);
  }
  return pso;
}

void save_pipeline_cache() {
  Cache& c = cache();
  std::lock_guard g(c.lock);
  save_locked();
}

std::string pipeline_cache_counts() {
  Cache& c = cache();
  return "pipelines loaded from the library " + std::to_string(c.loaded.load()) + ", compiled " + std::to_string(c.made.load());
}

}  // namespace am::rt::d3d12x
