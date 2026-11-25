// Pipeline states. Each keeps its root signature and copies of its shaders' bytecode (the
// console's compiled containers), which the shader translator turns into host pipelines.
//
// Made by:
// * device slot 62: compute pipelines, (D3D12_COMPUTE_PIPELINE_STATE_DESC, two arguments the
//   callers seen so far pass as 0, riid, out);
// * device slot 86: D3D12's CreatePipelineState, a stream of subobjects;
// * device slots 55, 57 and 67: the console's own creations, whose descriptions are not read yet
//   (the objects are made so the game's pipeline cache holds something; their first bytes are
//   logged).

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <type_traits>
#include <condition_variable>
#include <deque>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "runtime/d3d12x/d3d12x.h"
#include "host/dxc_compile.h"
#include "runtime/d3d12x/gpu_memory.h"
#include "runtime/d3d12x/host.h"
#include "shader/container.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"
#include "runtime/present/host_gpu.h"
#include "runtime/process.h"

namespace am::rt::d3d12x {
namespace {

struct Shader {
  uint32_t stage = 0;  // the stream's subobject type: 1 VS, 2 PS, 3 DS, 4 HS, 5 GS, 6 CS, 24 AS, 25 MS
  std::vector<uint8_t> bytecode;
};

// Slot 57: what a console pipeline's host pipelines are made from, at their first draw (one for
// each set of render-target formats it is drawn with).
struct ConsolePipeline {
  std::vector<std::string> markers;  // its GPU programs, in order (precompiled DXIL by marker)
  std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
  std::vector<std::string> names;  // the elements' semantic names
  bool layout_ok = false;
  uint32_t blend[8] = {};  // CB_BLEND0..7_CONTROL, from the first metadata block
  bool has_blend_desc = false;  // a derived pipeline's blend state (slot 55), in place of `blend`
  D3D12_BLEND_DESC blend_desc{};
  std::mutex lock;
  bool failed = false;  // its programs are not all precompiled: not tried again
  std::map<std::vector<uint32_t>, Microsoft::WRL::ComPtr<ID3D12PipelineState>> made;
};

struct PipelineState : Object {
  const char* made_by = "";
  Object* root_signature = nullptr;  // referenced
  std::vector<Shader> shaders;
  uint64_t extra[2] = {};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> host;  // when its shaders are precompiled
  bool compute = false;
  std::atomic<ID3D12PipelineState*> host_compute{nullptr};  // made on a worker thread, owned by `host`
  std::string compute_marker;           // its program's
  std::vector<UserSlot> compute_layout;  // where its root parameters go (user-data dwords)
  std::atomic<bool> compute_tried{false};  // a job has run (made or refused)
  std::atomic<bool> compute_urgent{false};  // a dispatch waited: moved to the front once
  uint32_t console_programs = 0;  // slot 57: GPU programs given by address
  std::string console_name;       // slot 57: the saved blocks' file, and the programs' markers
  std::unique_ptr<ConsolePipeline> console;

  ~PipelineState() {
    if (root_signature) release(root_signature);
  }
};

ID3D12PipelineState* console_host_pipeline(ConsolePipeline& c, const DXGI_FORMAT* targets, uint32_t count, DXGI_FORMAT depth,
                                           bool rect_list);

const Kind& pipeline_kind() {
  static const Kind k("pipeline state", 16, {});
  return k;
}

// The digest in a shader container's HASH part, as text ("" without one).
std::string container_hash(const uint8_t* b, size_t size) {
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= size) std::memcpy(&v, b + at, 4);
    return v;
  };
  if (size < 32 || std::memcmp(b, "DXBC", 4) != 0) return {};
  const uint32_t parts = u32(28);
  for (uint32_t i = 0; i < parts && i < 64; ++i) {
    const uint32_t at = u32(32 + size_t{i} * 4);
    if (u32(at) == 0x48534148 && at + 8 + 20 <= size) {  // "HASH": flags, then 16 bytes
      char text[33];
      for (int k = 0; k < 16; ++k) std::snprintf(text + 2 * k, 3, "%02x", b[at + 12 + k]);
      return text;
    }
  }
  return {};
}

// Every shader the title hands over is kept in the cache (cache/shaders/runtime under the
// output folder), once: shaders.rpf is not the only source of them (the boot screen's are
// elsewhere), and the translator wants exactly what the game uses. Containers are named by
// their HASH digest; the console's own pipelines (slot 57) by a digest of their blocks.
std::filesystem::path runtime_shader_dir() {
  static const std::filesystem::path dir = [] {
    std::filesystem::path d;
    if (TitleProcess* p = process(); p && !p->cache_dir().empty()) {
      d = p->cache_dir() / "shaders" / "runtime";
      std::error_code ec;
      std::filesystem::create_directories(d, ec);
      if (ec) d.clear();
    }
    return d;
  }();
  return dir;
}

void save_once(const std::string& name, const uint8_t* bytes, size_t size) {
  static std::mutex lock;
  static std::set<std::string> saved;
  const std::filesystem::path dir = runtime_shader_dir();
  if (dir.empty() || name.empty()) return;
  std::lock_guard g(lock);
  if (!saved.insert(name).second) return;
  const std::filesystem::path file = dir / name;
  std::error_code ec;
  if (std::filesystem::exists(file, ec)) return;
  std::ofstream out(file, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
}

PipelineState* new_pipeline(const char* made_by, Object* root_signature) {
  auto* p = make_object<PipelineState>(pipeline_kind());
  p->made_by = made_by;
  if (root_signature && is_object(root_signature)) {
    p->root_signature = root_signature;
    add_ref(root_signature);
  }
  return p;
}

std::string container_hash(const uint8_t* b, size_t size);

void add_shader(PipelineState* p, uint32_t stage, const D3D12_SHADER_BYTECODE& b) {
  if (!b.pShaderBytecode || !b.BytecodeLength) return;
  const auto* bytes = static_cast<const uint8_t*>(b.pShaderBytecode);
  p->shaders.push_back({stage, std::vector<uint8_t>(bytes, bytes + b.BytecodeLength)});
  const std::string hash = container_hash(bytes, b.BytecodeLength);
  if (!hash.empty()) save_once(hash + ".dxbc", bytes, b.BytecodeLength);
}

// The bytes a stream subobject takes: its type, its data at the data's alignment, the whole
// rounded up to a pointer's size (the stream's layout in D3D12). 0 for types we do not know.
template <class T>
constexpr size_t entry() {
  constexpr size_t data_at = (sizeof(UINT) + alignof(T) - 1) / alignof(T) * alignof(T);
  return (data_at + sizeof(T) + sizeof(void*) - 1) / sizeof(void*) * sizeof(void*);
}
template <class T>
constexpr size_t data_offset() {
  return (sizeof(UINT) + alignof(T) - 1) / alignof(T) * alignof(T);
}

size_t subobject_size(uint32_t type) {
  switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return entry<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return entry<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return entry<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return entry<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return entry<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return entry<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return entry<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return entry<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return entry<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return entry<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return entry<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return entry<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return entry<D3D12_VIEW_INSTANCING_DESC>();
    default: return 0;
  }
}

bool is_shader(uint32_t type) {
  return (type >= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && type <= D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) ||
         type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
}

// Host compute pipelines are made by two worker threads, in the order asked for: the driver can
// take very long over some programs (the game's watchdog ended the run while one was made on
// its thread), and the game asks for about 2,700 at start-up (a thread each ran the driver out
// of memory).
class PipelineMaker {
 public:
  explicit PipelineMaker(int threads) : threads_(threads) {}
  void add(std::function<void()> job, bool front = false) {
    std::lock_guard g(lock_);
    if (!started_) {
      started_ = true;
      for (int k = 0; k < threads_; ++k) {
        std::thread([this] {
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
          for (;;) {
            std::function<void()> next;
            {
              std::unique_lock u(lock_);
              wake_.wait(u, [this] { return !jobs_.empty(); });
              next = std::move(jobs_.front());
              jobs_.pop_front();
            }
            next();
          }
        }).detach();
      }
    }
    if (front) jobs_.push_front(std::move(job));
    else jobs_.push_back(std::move(job));
    wake_.notify_one();
  }

 private:
  std::mutex lock_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool started_ = false;
  int threads_;
};

std::atomic<uint64_t> g_compute_queued{0}, g_compute_made{0}, g_compute_missing{0}, g_compute_refused{0};

PipelineMaker& pipeline_maker() {
  static PipelineMaker* const m = new PipelineMaker(2);
  return *m;
}
// Programs over kLargeDxil bytes are made on a lane of their own: the driver can take minutes
// over one (a 1.4 MB program held both workers, and every pipeline after it, for good).
constexpr size_t kLargeDxil = 256 << 10;
PipelineMaker& large_pipeline_maker() {
  static PipelineMaker* const m = new PipelineMaker(1);
  return *m;
}

// One host pipeline a program: pipelines sharing a program share it, and it is made once.
struct MadeCompute {
  std::once_flag once;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  HRESULT hr = E_FAIL;
};
std::shared_ptr<MadeCompute> made_compute(const std::string& marker) {
  static std::mutex lock;
  static std::map<std::string, std::shared_ptr<MadeCompute>> made;
  std::lock_guard g(lock);
  auto& m = made[marker];
  if (!m) m = std::make_shared<MadeCompute>();
  return m;
}

// A compute pipeline's host pipeline: its program's precompiled translation (by marker) with the
// translated root signature, made by the pipeline maker; dispatches wait until it is there. A
// pipeline a dispatch waits for is queued again at the front (the job that runs second finds it
// made and does nothing).
void make_host_compute(PipelineState* p, present::HostGpu* gpu, bool large_lane) {
  const std::string& marker = p->compute_marker;
  std::vector<uint8_t> code;
  if (!load_precompiled(marker, code)) {  // a dispatch that waits for it lists it for the precompile
    ++g_compute_missing;
    release(p);
    return;
  }
  if (code.size() > kLargeDxil && !large_lane) {
    large_pipeline_maker().add([p, gpu] { make_host_compute(p, gpu, true); });
    return;
  }
  const std::shared_ptr<MadeCompute> m = made_compute(marker);
  std::call_once(m->once, [&] {
    D3D12_COMPUTE_PIPELINE_STATE_DESC host{};
    host.pRootSignature = translated_root_signature();
    host.CS = {code.data(), code.size()};
    const ULONGLONG start = GetTickCount64();
    m->hr = host.pRootSignature ? gpu->device->CreateComputePipelineState(&host, IID_PPV_ARGS(&m->pso)) : E_FAIL;
    const ULONGLONG took = GetTickCount64() - start;
    ++(SUCCEEDED(m->hr) ? g_compute_made : g_compute_refused);
    static std::atomic<int> told{0};
    if ((FAILED(m->hr) || took > 2000) && told.fetch_add(1, std::memory_order_relaxed) < 16) {
      logf("gpu: compute pipeline for %s (%zu bytes of DXIL) %s in %llu ms (0x%08lx)", marker.c_str(), code.size(),
           SUCCEEDED(m->hr) ? "made" : "refused", static_cast<unsigned long long>(took), static_cast<unsigned long>(m->hr));
    }
  });
  if (SUCCEEDED(m->hr)) {
    p->host = m->pso;
    p->host_compute.store(p->host.Get(), std::memory_order_release);
  }
  release(p);
}

// Queues the making of a compute pipeline's host pipeline (its program's precompiled
// translation with the translated root signature); dispatches wait until it is there. A
// pipeline a dispatch waits for is queued again at the front (the job that runs second finds it
// tried and does nothing).
void queue_host_compute(PipelineState* p, bool front) {
  present::HostGpu* gpu = present::host_gpu();
  if (!gpu || p->compute_marker.empty()) return;
  add_ref(p);  // kept alive until its job is done
  ++g_compute_queued;
  pipeline_maker().add([p, gpu] {
    if (p->compute_tried.exchange(true)) {
      release(p);
      return;
    }
    make_host_compute(p, gpu, false);
  }, front);
}

void start_host_compute(PipelineState* p, const std::string& marker) {
  p->compute = true;
  p->compute_marker = marker;
  queue_host_compute(p, false);
}

void log_first_bytes(const char* how, const void* desc, const PipelineState* p) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) >= 24) return;
  uint32_t w[8] = {};
  if (desc) std::memcpy(w, desc, sizeof w);
  logf("d3d12x: %s: description %08x %08x %08x %08x %08x %08x %08x %08x = %p", how, w[0], w[1], w[2], w[3], w[4], w[5],
       w[6], w[7], static_cast<const void*>(p));
}

}  // namespace

std::string compute_pipeline_counts() {
  return std::to_string(g_compute_made.load()) + " made, " + std::to_string(g_compute_missing.load()) + " not precompiled, " +
         std::to_string(g_compute_refused.load()) + " refused, of " + std::to_string(g_compute_queued.load()) + " queued";
}

std::string describe_pipeline(const void* object) {
  Object* o = object_of_kind(object, pipeline_kind());
  if (!o) return object ? "(not a pipeline of ours)" : "(none)";
  const auto* p = static_cast<const PipelineState*>(o);
  std::string text = p->made_by;
  static const char* const kStages[] = {"", "VS", "PS", "DS", "HS", "GS", "CS"};
  for (const Shader& s : p->shaders) {
    const char* stage = s.stage < 7 ? kStages[s.stage] : s.stage == 24 ? "AS" : s.stage == 25 ? "MS" : "?";
    text += std::string(" ") + stage + " " + container_hash(s.bytecode.data(), s.bytecode.size()).substr(0, 12);
  }
  if (!p->console_name.empty()) text += " blocks " + p->console_name;
  return text;
}

ID3D12PipelineState* host_pipeline(const void* object, const void** root_signature, const DXGI_FORMAT* targets,
                                   uint32_t target_count, DXGI_FORMAT depth, bool rect_list) {
  Object* o = object_of_kind(object, pipeline_kind());
  if (!o) return nullptr;
  auto* p = static_cast<PipelineState*>(o);
  if (root_signature) *root_signature = p->root_signature;
  if (p->compute) return nullptr;  // compute pipelines: host_compute_pipeline
  if (p->host) return p->host.Get();
  return p->console ? console_host_pipeline(*p->console, targets, target_count, depth, rect_list) : nullptr;
}

const std::vector<UserSlot>* compute_user_layout(const void* object) {
  Object* o = object_of_kind(object, pipeline_kind());
  if (!o) return nullptr;
  const auto* p = static_cast<const PipelineState*>(o);
  return p->compute_layout.empty() ? nullptr : &p->compute_layout;
}

ID3D12PipelineState* host_compute_pipeline(const void* object, const void** root_signature) {
  Object* o = object_of_kind(object, pipeline_kind());
  if (!o) return nullptr;
  auto* p = static_cast<PipelineState*>(o);
  if (!p->compute) return nullptr;
  if (root_signature) *root_signature = p->root_signature;
  ID3D12PipelineState* host = p->host_compute.load(std::memory_order_acquire);
  if (!host && !p->compute_tried.load() && !p->compute_urgent.exchange(true)) queue_host_compute(p, true);  // a dispatch waits
  if (!host && p->compute_tried.load()) note_wanted(p->compute_marker);  // tried, nothing made: not precompiled (or refused)
  return host;
}

HRESULT create_compute_pipeline(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, uint64_t extra0, uint64_t extra1,
                                const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!desc || !desc->CS.pShaderBytecode || !desc->CS.BytecodeLength) return E_INVALIDARG;
  PipelineState* p = new_pipeline("compute (slot 62)", reinterpret_cast<Object*>(desc->pRootSignature));
  add_shader(p, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, desc->CS);
  p->extra[0] = extra0;
  p->extra[1] = extra1;
  p->compute = true;
  start_host_compute(p, program_marker(p->shaders[0].bytecode, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS));
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    const Shader& cs = p->shaders[0];
    logf("d3d12x: compute pipeline (root signature %p, %zu bytes of shader, hash %s, extra %llx %llx) = %p",
         static_cast<void*>(p->root_signature), cs.bytecode.size(), container_hash(cs.bytecode.data(), cs.bytecode.size()).c_str(),
         static_cast<unsigned long long>(extra0), static_cast<unsigned long long>(extra1), static_cast<void*>(p));
  }
  *out = p;
  return S_OK;
}

HRESULT create_pipeline_from_stream(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!desc || !desc->pPipelineStateSubobjectStream || !desc->SizeInBytes) return E_INVALIDARG;
  const auto* stream = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
  PipelineState* p = new_pipeline("stream (slot 86)", nullptr);
  std::string types;
  size_t at = 0;
  // The host description, with D3D12's defaults where the stream says nothing.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC host{};
  host.SampleMask = UINT_MAX;
  host.SampleDesc.Count = 1;
  host.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, 0,
                          D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
  for (auto& rt : host.BlendState.RenderTarget) rt = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                                      D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP,
                                                      D3D12_COLOR_WRITE_ENABLE_ALL};
  host.DepthStencilState.DepthEnable = TRUE;
  host.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  host.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  host.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
  std::vector<std::string> names;
  names.reserve(32);
  const auto read = [&](auto& value) { std::memcpy(&value, stream + at + data_offset<std::remove_reference_t<decltype(value)>>(), sizeof value); };
  while (at + sizeof(UINT) <= desc->SizeInBytes) {
    uint32_t type = 0;
    std::memcpy(&type, stream + at, sizeof type);
    const size_t size = subobject_size(type);
    types += (types.empty() ? "" : " ") + std::to_string(type);
    if (!size || at + size > desc->SizeInBytes) {
      if (!size) types += "?";  // a type we do not know: the rest of the stream is not read
      break;
    }
    if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
      Object* rs = nullptr;
      std::memcpy(&rs, stream + at + data_offset<ID3D12RootSignature*>(), sizeof rs);
      if (rs && is_object(rs) && !p->root_signature) {
        p->root_signature = rs;
        add_ref(rs);
      }
    } else if (is_shader(type)) {
      D3D12_SHADER_BYTECODE b{};
      std::memcpy(&b, stream + at + data_offset<D3D12_SHADER_BYTECODE>(), sizeof b);
      add_shader(p, type, b);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND) {
      read(host.BlendState);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK) {
      read(host.SampleMask);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER) {
      read(host.RasterizerState);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL) {
      read(host.DepthStencilState);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1) {
      D3D12_DEPTH_STENCIL_DESC1 d1{};
      read(d1);
      std::memcpy(&host.DepthStencilState, &d1, sizeof host.DepthStencilState);  // the same fields, then DepthBoundsTestEnable
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
      D3D12_INPUT_LAYOUT_DESC il{};
      read(il);
      for (UINT k = 0; il.pInputElementDescs && k < il.NumElements && k < 32; ++k) {
        D3D12_INPUT_ELEMENT_DESC e = il.pInputElementDescs[k];
        names.emplace_back(e.SemanticName ? e.SemanticName : "");
        elements.push_back(e);
      }
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE) {
      read(host.IBStripCutValue);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) {
      read(host.PrimitiveTopologyType);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS) {
      D3D12_RT_FORMAT_ARRAY f{};
      read(f);
      host.NumRenderTargets = std::min<UINT>(f.NumRenderTargets, 8);
      for (UINT k = 0; k < host.NumRenderTargets; ++k) host.RTVFormats[k] = f.RTFormats[k];
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT) {
      read(host.DSVFormat);
    } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC) {
      read(host.SampleDesc);
      host.SampleDesc = {1, 0};  // the host's targets are single-sampled (host_texture)
    }
    at += size;
  }
  // The host pipeline, when every shader has been compiled ahead of time.
  std::vector<std::vector<uint8_t>> dxil;
  bool all = !p->shaders.empty();
  for (const Shader& sh : p->shaders) {
    std::vector<uint8_t> code;
    if (sh.stage != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS && sh.stage != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) all = false;
    if (!all || !load_precompiled(program_marker(sh.bytecode, sh.stage), code)) {
      all = false;
      break;
    }
    (sh.stage == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ? host.VS : host.PS) = {nullptr, 0};
    dxil.push_back(std::move(code));
    D3D12_SHADER_BYTECODE& b = sh.stage == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ? host.VS : host.PS;
    b = {dxil.back().data(), dxil.back().size()};
  }
  if (all) {
    for (size_t k = 0; k < elements.size(); ++k) elements[k].SemanticName = names[k].c_str();
    host.InputLayout = {elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size())};
    // The host's typeless or sRGB render-target formats come from the title's view at draw time;
    // the pipeline takes the description's.
    p->host.Attach(make_host_graphics_pipeline(host, "a stream pipeline"));
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("d3d12x: CreatePipelineState(%zu bytes of stream: subobjects %s; %zu shaders) = %p", desc->SizeInBytes,
         types.c_str(), p->shaders.size(), static_cast<void*>(p));
  }
  *out = p;
  return S_OK;
}

namespace {

// The stage a shader container holds, by its DXIL part's program header (kind << 16 | version),
// as a stream subobject type; 0 when unknown.
uint32_t container_stage(const uint8_t* b, size_t size) {
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= size) std::memcpy(&v, b + at, 4);
    return v;
  };
  const uint32_t parts = u32(28);
  for (uint32_t i = 0; i < parts && i < 64; ++i) {
    const uint32_t at = u32(32 + size_t{i} * 4);
    if (u32(at) == 0x4C495844) {  // "DXIL"
      switch (u32(at + 8) >> 16) {
        case 0: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
        case 1: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
        case 2: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS;
        case 3: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS;
        case 4: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS;
        case 5: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
        case 13: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
        case 14: return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
        default: return 0;
      }
    }
  }
  return 0;
}

bool read_guarded(const void* from, void* to, size_t size) {
  __try {
    std::memcpy(to, from, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A shader container at `address`, if there is one: its bytes.
bool container_at(uint64_t address, std::vector<uint8_t>& out) {
  if (address < 0x10000 || (address & 3)) return false;
  uint32_t head[8] = {};
  if (!read_guarded(reinterpret_cast<const void*>(address), head, sizeof head) || head[0] != 0x43425844) return false;  // "DXBC"
  const uint32_t size = head[6];
  if (size < 32 || size > (64u << 20)) return false;
  out.resize(size);
  return read_guarded(reinterpret_cast<const void*>(address), out.data(), size);
}

// The console's pipeline descriptions are not read yet, but their shaders can be found: pointers
// to shader containers in the description's first 32 words, or one pointer further.
void find_shaders(PipelineState* p, const void* desc) {
  uint64_t words[32] = {};
  if (!desc || !read_guarded(desc, words, sizeof words)) return;
  std::vector<uint8_t> bytes;
  auto take = [&](uint64_t address) {
    if (!container_at(address, bytes)) return false;
    for (const Shader& s : p->shaders) {
      if (s.bytecode == bytes) return true;
    }
    p->shaders.push_back({container_stage(bytes.data(), bytes.size()), bytes});
    return true;
  };
  for (uint64_t w : words) {
    if (take(w)) continue;
    uint64_t inner[8] = {};
    if (w >= 0x10000 && !(w & 7) && read_guarded(reinterpret_cast<const void*>(w), inner, sizeof inner)) {
      for (uint64_t v : inner) take(v);
    }
  }
}

// A console GPU program begins with a 24-byte marker: two VOP3 v_nops with literals.
bool is_console_program(const uint8_t* b, size_t size) {
  if (size < 24) return false;
  uint32_t w[6];
  std::memcpy(w, b, sizeof w);
  return (w[0] & 0xFFFF0000u) == 0xD5800000u && (w[3] & 0xFFFF0000u) == 0xD5800000u;
}

uint64_t fnv1a(const uint8_t* b, size_t size, uint64_t h = 0xCBF29CE484222325ull) {
  for (size_t i = 0; i < size; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
  return h;
}

// The second metadata block of a slot 57 pipeline holds its input layout: at 0x4C the count, then
// for each element its semantic name (a length, then the bytes with their NUL) and the six fields
// of a D3D12_INPUT_ELEMENT_DESC after it (index, format, slot, offset, class, step rate).
void read_input_layout(const std::vector<uint8_t>& b, ConsolePipeline& c) {
  const auto u32 = [&](size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
  };
  if (b.size() < 0x50) return;
  const uint32_t count = u32(0x4C);
  size_t at = 0x50;
  if (count > 32) return;
  for (uint32_t k = 0; k < count; ++k) {
    if (at + 4 > b.size()) return;
    const uint32_t len = u32(at);
    at += 4;
    if (!len || len > 64 || at + len + 24 > b.size()) return;
    c.names.emplace_back(reinterpret_cast<const char*>(b.data() + at), strnlen(reinterpret_cast<const char*>(b.data() + at), len));
    at += len;
    D3D12_INPUT_ELEMENT_DESC e{};
    e.SemanticIndex = u32(at);
    e.Format = static_cast<DXGI_FORMAT>(u32(at + 4));
    e.InputSlot = u32(at + 8);
    e.AlignedByteOffset = u32(at + 12);
    e.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(u32(at + 16));
    e.InstanceDataStepRate = u32(at + 20);
    at += 24;
    c.elements.push_back(e);
  }
  c.layout_ok = true;
}

// An AMD blend control (CB_BLENDn_CONTROL) as D3D12's: factors, functions, separate alpha, enable.
D3D12_BLEND blend_factor(uint32_t f, bool alpha) {
  switch (f) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 2: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 3: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 6: return D3D12_BLEND_DEST_ALPHA;
    case 7: return D3D12_BLEND_INV_DEST_ALPHA;
    case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 13: case 19: return D3D12_BLEND_BLEND_FACTOR;
    case 14: case 20: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 15: return alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
    case 16: return alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
    case 17: return D3D12_BLEND_SRC1_ALPHA;
    case 18: return D3D12_BLEND_INV_SRC1_ALPHA;
    default: return D3D12_BLEND_ONE;
  }
}
D3D12_BLEND_OP blend_op(uint32_t f) {
  switch (f) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}
D3D12_RENDER_TARGET_BLEND_DESC blend_of(uint32_t v) {
  D3D12_RENDER_TARGET_BLEND_DESC d{FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                                   D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
  if (!((v >> 30) & 1)) return d;
  const bool separate = (v >> 29) & 1;
  d.BlendEnable = TRUE;
  d.SrcBlend = blend_factor(v & 31, false);
  d.BlendOp = blend_op((v >> 5) & 7);
  d.DestBlend = blend_factor((v >> 8) & 31, false);
  d.SrcBlendAlpha = blend_factor(separate ? (v >> 16) & 31 : v & 31, true);
  d.BlendOpAlpha = blend_op(separate ? (v >> 21) & 7 : (v >> 5) & 7);
  d.DestBlendAlpha = blend_factor(separate ? (v >> 24) & 31 : (v >> 8) & 31, true);
  return d;
}

// The shader kind in a DXIL container's program header (0 pixel, 1 vertex, ...); -1 without one.
int dxil_kind(const std::vector<uint8_t>& dxil) {
  std::string error;
  const auto c = shader::parse_container(dxil, error);
  const shader::Part* part = c ? c->find("DXIL") : nullptr;
  if (!part || part->data.size() < 4) return -1;
  uint32_t version = 0;
  std::memcpy(&version, part->data.data(), 4);
  return static_cast<int>(version >> 16);
}

// The vertex stage's outputs, from its DXIL's output signature (OSG1): semantic names and indices,
// in order; empty when it cannot be read. Translated programs declare every output as a float4.
struct SignatureElement {
  std::string name;
  uint32_t index = 0, system_value = 0;
};
std::vector<SignatureElement> output_signature(const std::vector<uint8_t>& dxil) {
  std::vector<SignatureElement> out;
  std::string error;
  const auto c = shader::parse_container(dxil, error);
  const shader::Part* part = c ? c->find("OSG1") : nullptr;
  if (!part || part->data.size() < 8) return out;
  const auto& d = part->data;
  const auto u32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  };
  const uint32_t count = u32(0), first = u32(4);
  for (uint32_t k = 0; k < count && k < 32; ++k) {
    const size_t at = first + size_t{k} * 32;  // stream, name, index, system value, type, register, masks, precision
    if (at + 32 > d.size()) return {};
    SignatureElement e;
    const uint32_t name_at = u32(at + 4);
    if (name_at >= d.size()) return {};
    e.name.assign(reinterpret_cast<const char*>(d.data() + name_at), strnlen(reinterpret_cast<const char*>(d.data() + name_at), d.size() - name_at));
    e.index = u32(at + 8);
    e.system_value = u32(at + 12);
    out.push_back(std::move(e));
  }
  return out;
}

// The rect list's fourth corner: a geometry shader that finds each triangle's right-angle corner
// k (the one facing its longest edge, on screen), and draws k, a, b and a + b - k (every output
// alike) as a strip of two triangles covering the rectangle.
std::vector<uint8_t> rect_list_geometry_shader(const std::vector<uint8_t>& vs) {
  const std::vector<SignatureElement> outputs = output_signature(vs);
  if (outputs.empty()) return {};
  std::string decl = "struct V {\n", corner, position;
  for (size_t k = 0; k < outputs.size(); ++k) {
    const std::string field = "f" + std::to_string(k);
    const std::string semantic = outputs[k].system_value == 1 ? "SV_Position" : outputs[k].name + std::to_string(outputs[k].index);
    decl += "  float4 " + field + " : " + semantic + ";\n";
    corner += "  d." + field + " = a." + field + " + b." + field + " - k." + field + ";\n";
    if (outputs[k].system_value == 1) position = field;
  }
  if (position.empty()) return {};
  decl += "};\n";
  const auto at = [&](int n) { return "v[" + std::to_string(n) + "]." + position + ".xy / v[" + std::to_string(n) + "]." + position + ".w"; };
  const std::string hlsl =
      decl + "[maxvertexcount(4)] void main(triangle V v[3], inout TriangleStream<V> s) {\n" +
      "  float2 p0 = " + at(0) + ", p1 = " + at(1) + ", p2 = " + at(2) + ";\n" +
      "  float e0 = distance(p1, p2), e1 = distance(p2, p0), e2 = distance(p0, p1);\n" +
      "  V k = v[2], a = v[0], b = v[1];\n" +
      "  if (e0 >= e1 && e0 >= e2) { k = v[0]; a = v[1]; b = v[2]; }\n" +
      "  else if (e1 >= e2) { k = v[1]; a = v[2]; b = v[0]; }\n" +
      "  V d;\n" + corner + "  s.Append(k); s.Append(a); s.Append(b); s.Append(d);\n}\n";
  const host::CompileResult r = host::compile_hlsl(hlsl, "main", "gs_6_0");
  if (!r.ok) logf("gpu: the rect list's geometry shader did not compile: %s", r.messages.substr(0, 300).c_str());
  return r.ok ? r.dxil : std::vector<uint8_t>{};
}

// A slot 57 pipeline's host pipeline for these render-target formats: its precompiled vertex and
// pixel programs, its input layout and blend; no culling and no depth until their registers are
// read.
ID3D12PipelineState* console_host_pipeline(ConsolePipeline& c, const DXGI_FORMAT* targets, uint32_t count, DXGI_FORMAT depth,
                                           bool rect_list) {
  std::lock_guard g(c.lock);
  if (c.failed) return nullptr;
  std::vector<uint32_t> key(targets, targets + std::min<uint32_t>(count, 8));
  key.push_back(rect_list);
  key.push_back(depth);  // last: the depth view's format (the pipeline must match the bound views)
  if (auto it = c.made.find(key); it != c.made.end()) return it->second.Get();
  std::vector<uint8_t> vs, ps;
  for (const std::string& m : c.markers) {
    std::vector<uint8_t> code;
    if (!load_precompiled(m, code)) {
      for (const std::string& each : c.markers) note_wanted(each);  // a draw waits for them
      c.failed = true;
      return nullptr;
    }
    const int kind = dxil_kind(code);
    if (kind == 1 && vs.empty()) vs = std::move(code);
    else if (kind == 0 && ps.empty()) ps = std::move(code);
    else {
      c.failed = true;  // a stage we do not make yet (mesh, geometry, ...)
      return nullptr;
    }
  }
  if (vs.empty() || !c.layout_ok) {
    c.failed = true;
    return nullptr;
  }
  // AMERICAS_DEBUG_PS: a solid magenta pixel shader in place of the program's, to tell the
  // vertex side's faults from the pixel side's.
  static const std::vector<uint8_t> debug_ps = [] {
    char* v = nullptr;
    size_t n = 0;
    const bool on = _dupenv_s(&v, &n, "AMERICAS_DEBUG_PS") == 0 && v;
    std::free(v);
    if (!on) return std::vector<uint8_t>{};
    const host::CompileResult r = host::compile_hlsl("float4 main() : SV_Target { return float4(1, 0, 1, 1); }", "main", "ps_6_6");
    logf("gpu: debug pixel shader %s", r.ok ? "on" : r.messages.c_str());
    return r.ok ? r.dxil : std::vector<uint8_t>{};
  }();
  if (!debug_ps.empty()) ps = debug_ps;
  // AMERICAS_DEBUG_VS: a vertex shader that places the vertex buffer's pixel positions on a
  // 3840x2160 screen itself (no memory reads), to tell the input path's faults from the program's.
  static const std::vector<uint8_t> debug_vs = [] {
    if (!GetEnvironmentVariableA("AMERICAS_DEBUG_VS", nullptr, 0)) return std::vector<uint8_t>{};
    const host::CompileResult r = host::compile_hlsl(
        (GetEnvironmentVariableA("AMERICAS_DEBUG_VS_ID", nullptr, 0) ? "struct I { float3 p : POSITION0; float4 c : COLOR0; float2 t : TEXCOORD0; };\n float4 main(I i, uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.5, 1.0); }" : "struct I { float3 p : POSITION0; float4 c : COLOR0; float2 t : TEXCOORD0; };\n float4 main(I i) : SV_Position { return float4(i.p.x / 1920.0 - 1.0, 1.0 - i.p.y / 1080.0, 0.5, 1.0); }"),
        "main", "vs_6_6");
    logf("gpu: debug vertex shader %s", r.ok ? "on" : r.messages.c_str());
    return r.ok ? r.dxil : std::vector<uint8_t>{};
  }();
  if (!debug_vs.empty()) vs = debug_vs;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.VS = {vs.data(), vs.size()};
  if (!ps.empty()) d.PS = {ps.data(), ps.size()};
  d.SampleMask = UINT_MAX;
  d.SampleDesc.Count = 1;
  d.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, 0,
                       D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
  d.BlendState.IndependentBlendEnable = TRUE;
  for (int k = 0; k < 8; ++k) d.BlendState.RenderTarget[k] = blend_of(c.blend[k]);
  if (c.has_blend_desc) d.BlendState = c.blend_desc;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = static_cast<UINT>(key.size() - 2);
  for (size_t k = 0; k + 2 < key.size(); ++k) d.RTVFormats[k] = static_cast<DXGI_FORMAT>(key[k]);
  std::vector<uint8_t> gs;
  if (rect_list) {
    gs = rect_list_geometry_shader(vs);
    if (gs.empty()) {
      c.made[key] = nullptr;
      return nullptr;
    }
    d.GS = {gs.data(), gs.size()};
  }
  d.DSVFormat = depth;  // depth testing stays off until the depth registers are read
  std::vector<D3D12_INPUT_ELEMENT_DESC> elements = c.elements;
  for (size_t k = 0; k < elements.size(); ++k) elements[k].SemanticName = c.names[k].c_str();
  d.InputLayout = {elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size())};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  pso.Attach(make_host_graphics_pipeline(d, "a console pipeline (slot 57)"));
  c.made[key] = pso;
  return pso.Get();
}

// Slot 57's description: the root signature, then (address, size) pairs: blocks of metadata in
// title memory and GPU programs already in GPU memory, each program after its metadata. The
// pairs end at the first one that is neither. Saved as one file: for each block its address
// (8 bytes), size and kind (4 each; kind 1 for a program), then its bytes.
void capture_console_blocks(PipelineState* p, const void* desc, bool compute) {
  uint64_t words[33] = {};
  if (!desc || !read_guarded(desc, words, sizeof words)) return;
  if (is_object(reinterpret_cast<void*>(words[0])) && !p->root_signature) {
    p->root_signature = reinterpret_cast<Object*>(words[0]);
    add_ref(p->root_signature);
  }
  std::vector<uint8_t> bundle, block;
  uint32_t programs = 0;
  std::string markers;
  auto console = std::make_unique<ConsolePipeline>();
  int metadata = 0;
  for (int i = 1; i + 1 < 33; i += 2) {
    const uint64_t address = words[i], size = words[i + 1];
    if (!address && !size) continue;
    if (address < 0x10000 || !size || size > (1u << 20)) break;
    block.resize(size);
    if (!read_guarded(reinterpret_cast<const void*>(address), block.data(), size)) break;
    const uint32_t kind = is_console_program(block.data(), block.size()) ? 1 : 0;
    const uint32_t size32 = static_cast<uint32_t>(size);
    bundle.insert(bundle.end(), reinterpret_cast<const uint8_t*>(&address), reinterpret_cast<const uint8_t*>(&address) + 8);
    bundle.insert(bundle.end(), reinterpret_cast<const uint8_t*>(&size32), reinterpret_cast<const uint8_t*>(&size32) + 4);
    bundle.insert(bundle.end(), reinterpret_cast<const uint8_t*>(&kind), reinterpret_cast<const uint8_t*>(&kind) + 4);
    bundle.insert(bundle.end(), block.begin(), block.end());
    if (compute) {
      // Compute pipelines: no blend or input layout. The block after the program says where
      // the root parameters arrive: from +0x18, 32-byte records; tag 0x41 is a resource the
      // code reads: byte 5 its root type, byte 20 descriptor dwords, byte 21 the user-data
      // dword (sN for compute), byte 22 the root parameter; 0x43 ends the list.
      if (!kind && programs > 0 && p->compute_layout.empty() && block.size() >= 0x18 && block[0] == 0x03 && block[1] == 0x04) {
        for (size_t at = 0x18; at + 32 <= block.size(); at += 32) {
          if (block[at] == 0x43) break;
          if (block[at] != 0x41) continue;
          const UserSlot u{block[at + 5], block[at + 21], block[at + 22], block[at + 20]};
          bool seen = false;
          for (const UserSlot& e : p->compute_layout) seen = seen || (e.slot == u.slot && e.user == u.user);
          if (!seen) p->compute_layout.push_back(u);
        }
      }
    } else if (!kind && metadata == 0 && block.size() >= 0xF0) {
      std::memcpy(console->blend, block.data() + 0xD0, sizeof console->blend);
    } else if (!kind && metadata == 1) {
      read_input_layout(block, *console);
    }
    if (!kind) ++metadata;
    if (kind && block.size() >= 24) {
      static const char* const kHex = "0123456789abcdef";
      std::string m;
      for (size_t k = 0; k < 24; ++k) {
        m += kHex[block[k] >> 4];
        m += kHex[block[k] & 15];
      }
      console->markers.push_back(std::move(m));
    }
    if (kind) {
      ++programs;
      char m[32];
      uint32_t w[3];
      std::memcpy(w, block.data(), sizeof w);
      std::snprintf(m, sizeof m, " %08x%08x", w[1], w[2]);
      markers += m;
    }
  }
  if (bundle.empty()) return;
  char name[48];
  std::snprintf(name, sizeof name, "%016llx.cpipe", static_cast<unsigned long long>(fnv1a(bundle.data(), bundle.size())));
  save_once(name, bundle.data(), bundle.size());
  p->console_programs = programs;
  p->console_name = std::string(name, 16) + (markers.empty() ? "" : ":" + markers);
  if (compute) {
    if (console->markers.size() == 1) start_host_compute(p, console->markers[0]);
    else logf("d3d12x: a console compute pipeline with %zu programs (one expected)", console->markers.size());
    return;  // no graphics pipeline to make
  }
  p->console = std::move(console);
}

}  // namespace

HRESULT create_derived_pipeline(const void* base, UINT count, const void* desc, const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  Object* o = base ? object_of_kind(base, pipeline_kind()) : nullptr;
  if (!o) {
    logf("d3d12x: slot 55: the base %p is not one of our pipelines", base);
    return E_INVALIDARG;
  }
  const auto* b = static_cast<PipelineState*>(o);
  PipelineState* p = new_pipeline("pipeline (slot 55)", b->root_signature);
  p->shaders = b->shaders;
  p->console_programs = b->console_programs;
  p->console_name = b->console_name;
  p->host = b->host;  // a stream pipeline's: its blend state stays the base's for now
  if (b->console) {
    p->console = std::make_unique<ConsolePipeline>();
    p->console->markers = b->console->markers;
    p->console->elements = b->console->elements;
    p->console->names = b->console->names;
    p->console->layout_ok = b->console->layout_ok;
    std::copy(std::begin(b->console->blend), std::end(b->console->blend), p->console->blend);
    p->console->has_blend_desc = b->console->has_blend_desc;
    p->console->blend_desc = b->console->blend_desc;
  }
  // The replaced state: a type, then the state (2: blend). Only the first of `count` is read.
  uint32_t type = 0;
  read_guarded(desc, &type, sizeof type);
  D3D12_BLEND_DESC blend{};
  const bool is_blend = type == 2 && read_guarded(static_cast<const uint8_t*>(desc) + 4, &blend, sizeof blend);
  if (is_blend && p->console) {
    p->console->has_blend_desc = true;
    p->console->blend_desc = blend;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8 || (!is_blend && logged.load() < 64)) {
    logf("d3d12x: slot 55: %s derived (%u descriptions, the first of type %u)%s = %p", b->made_by, count, type,
         p->console || p->host ? "" : " (the base has no host pipeline)", static_cast<void*>(p));
  }
  *out = p;
  return S_OK;
}

HRESULT create_console_pipeline(const char* how, const void* desc, const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  PipelineState* p = new_pipeline(how, nullptr);
  // Slots 57 (graphics) and 67 (compute) describe their pipelines the same way: the root
  // signature object, then (address, size) blocks of metadata and GPU programs.
  if (std::strstr(how, "57")) capture_console_blocks(p, desc, false);
  else if (std::strstr(how, "67")) capture_console_blocks(p, desc, true);
  find_shaders(p, desc);
  log_first_bytes(how, desc, p);
  *out = p;
  return S_OK;
}

}  // namespace am::rt::d3d12x
