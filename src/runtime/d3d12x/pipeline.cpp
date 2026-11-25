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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/host.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"
#include "runtime/process.h"

namespace am::rt::d3d12x {
namespace {

struct Shader {
  uint32_t stage = 0;  // the stream's subobject type: 1 VS, 2 PS, 3 DS, 4 HS, 5 GS, 6 CS, 24 AS, 25 MS
  std::vector<uint8_t> bytecode;
};

struct PipelineState : Object {
  const char* made_by = "";
  Object* root_signature = nullptr;  // referenced
  std::vector<Shader> shaders;
  uint64_t extra[2] = {};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> host;  // when its shaders are precompiled
  uint32_t console_programs = 0;  // slot 57: GPU programs given by address
  std::string console_name;       // slot 57: the saved blocks' file, and the programs' markers

  ~PipelineState() {
    if (root_signature) release(root_signature);
  }
};

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

void log_first_bytes(const char* how, const void* desc, const PipelineState* p) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) >= 24) return;
  uint32_t w[8] = {};
  if (desc) std::memcpy(w, desc, sizeof w);
  logf("d3d12x: %s: description %08x %08x %08x %08x %08x %08x %08x %08x = %p", how, w[0], w[1], w[2], w[3], w[4], w[5],
       w[6], w[7], static_cast<const void*>(p));
}

}  // namespace

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

ID3D12PipelineState* host_pipeline(const void* object, const void** root_signature) {
  Object* o = object_of_kind(object, pipeline_kind());
  if (!o) return nullptr;
  auto* p = static_cast<PipelineState*>(o);
  if (root_signature) *root_signature = p->root_signature;
  return p->host.Get();
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

// Slot 57's description: the root signature, then (address, size) pairs: blocks of metadata in
// title memory and GPU programs already in GPU memory, each program after its metadata. The
// pairs end at the first one that is neither. Saved as one file: for each block its address
// (8 bytes), size and kind (4 each; kind 1 for a program), then its bytes.
void capture_console_blocks(PipelineState* p, const void* desc) {
  uint64_t words[33] = {};
  if (!desc || !read_guarded(desc, words, sizeof words)) return;
  if (is_object(reinterpret_cast<void*>(words[0])) && !p->root_signature) {
    p->root_signature = reinterpret_cast<Object*>(words[0]);
    add_ref(p->root_signature);
  }
  std::vector<uint8_t> bundle, block;
  uint32_t programs = 0;
  std::string markers;
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
}

}  // namespace

HRESULT create_console_pipeline(const char* how, const void* desc, const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  PipelineState* p = new_pipeline(how, nullptr);
  if (std::strstr(how, "57")) capture_console_blocks(p, desc);
  find_shaders(p, desc);
  log_first_bytes(how, desc, p);
  *out = p;
  return S_OK;
}

}  // namespace am::rt::d3d12x
