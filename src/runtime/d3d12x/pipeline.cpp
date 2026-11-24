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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

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

PipelineState* new_pipeline(const char* made_by, Object* root_signature) {
  auto* p = make_object<PipelineState>(pipeline_kind());
  p->made_by = made_by;
  if (root_signature && is_object(root_signature)) {
    p->root_signature = root_signature;
    add_ref(root_signature);
  }
  return p;
}

void add_shader(PipelineState* p, uint32_t stage, const D3D12_SHADER_BYTECODE& b) {
  if (!b.pShaderBytecode || !b.BytecodeLength) return;
  const auto* bytes = static_cast<const uint8_t*>(b.pShaderBytecode);
  p->shaders.push_back({stage, std::vector<uint8_t>(bytes, bytes + b.BytecodeLength)});
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
    }
    at += size;
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("d3d12x: CreatePipelineState(%zu bytes of stream: subobjects %s; %zu shaders) = %p", desc->SizeInBytes,
         types.c_str(), p->shaders.size(), static_cast<void*>(p));
  }
  *out = p;
  return S_OK;
}

HRESULT create_console_pipeline(const char* how, const void* desc, const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  PipelineState* p = new_pipeline(how, nullptr);
  log_first_bytes(how, desc, p);
  *out = p;
  return S_OK;
}

}  // namespace am::rt::d3d12x
