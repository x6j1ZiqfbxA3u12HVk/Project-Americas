// Pipeline states. Each keeps its root signature and a copy of its shaders' bytecode (the
// console's compiled containers), which the shader translator turns into host pipelines.
//
// Compute pipelines come from device slot 62: (D3D12_COMPUTE_PIPELINE_STATE_DESC, two arguments
// the callers seen so far pass as 0, riid, out).

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

struct PipelineState : Object {
  bool compute = false;
  Object* root_signature = nullptr;  // referenced
  std::vector<uint8_t> cs;
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

}  // namespace

HRESULT create_compute_pipeline(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, uint64_t extra0, uint64_t extra1,
                                const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!desc || !desc->CS.pShaderBytecode || !desc->CS.BytecodeLength) return E_INVALIDARG;
  auto* p = make_object<PipelineState>(pipeline_kind());
  p->compute = true;
  if (desc->pRootSignature) {
    p->root_signature = reinterpret_cast<Object*>(desc->pRootSignature);
    add_ref(p->root_signature);
  }
  const auto* cs = static_cast<const uint8_t*>(desc->CS.pShaderBytecode);
  p->cs.assign(cs, cs + desc->CS.BytecodeLength);
  p->extra[0] = extra0;
  p->extra[1] = extra1;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("d3d12x: compute pipeline (root signature %p, %zu bytes of shader, hash %s, extra %llx %llx) = %p",
         static_cast<void*>(p->root_signature), p->cs.size(), container_hash(cs, p->cs.size()).c_str(),
         static_cast<unsigned long long>(extra0), static_cast<unsigned long long>(extra1), static_cast<void*>(p));
  }
  *out = p;
  return S_OK;
}

}  // namespace am::rt::d3d12x
