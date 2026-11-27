#include "runtime/d3d12x/root_signature.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

// ---- blobs (ID3DBlob: slot 3 GetBufferPointer, slot 4 GetBufferSize) ----

struct Blob : Object {
  std::vector<uint8_t> bytes;
};

void* WINAPI blob_pointer(Blob* self) { return self->bytes.data(); }
SIZE_T WINAPI blob_size(Blob* self) { return self->bytes.size(); }

const Kind& blob_kind() {
  static const Kind k("blob", 8,
                      {
                          {3, "GetBufferPointer", reinterpret_cast<void*>(&blob_pointer)},
                          {4, "GetBufferSize", reinterpret_cast<void*>(&blob_size)},
                      });
  return k;
}

// ---- reading D3D12's descriptions ----

template <class Range>
RootRange range_from(const Range& r) {
  RootRange out;
  out.type = r.RangeType;
  out.count = r.NumDescriptors;
  out.base_register = r.BaseShaderRegister;
  out.space = r.RegisterSpace;
  if constexpr (requires { r.Flags; }) out.flags = r.Flags;
  out.table_offset = r.OffsetInDescriptorsFromTableStart;
  return out;
}

template <class Param>
bool parameter_from(const Param& p, RootParameter& out, std::string& error) {
  out.type = p.ParameterType;
  out.visibility = p.ShaderVisibility;
  switch (p.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      const auto& t = p.DescriptorTable;
      if (t.NumDescriptorRanges && !t.pDescriptorRanges) {
        error = "a descriptor table without ranges";
        return false;
      }
      for (UINT i = 0; i < t.NumDescriptorRanges; ++i) out.ranges.push_back(range_from(t.pDescriptorRanges[i]));
      return true;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      out.shader_register = p.Constants.ShaderRegister;
      out.space = p.Constants.RegisterSpace;
      out.constants = p.Constants.Num32BitValues;
      return true;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      out.shader_register = p.Descriptor.ShaderRegister;
      out.space = p.Descriptor.RegisterSpace;
      if constexpr (requires { p.Descriptor.Flags; }) out.flags = p.Descriptor.Flags;
      return true;
    default:
      error = "an unknown root parameter type " + std::to_string(p.ParameterType);
      return false;
  }
}

D3D12_STATIC_SAMPLER_DESC1 sampler_from(const D3D12_STATIC_SAMPLER_DESC& s) {
  D3D12_STATIC_SAMPLER_DESC1 out{};
  std::memcpy(&out, &s, sizeof s);  // the same fields, in the same order, before Flags
  return out;
}
D3D12_STATIC_SAMPLER_DESC1 sampler_from(const D3D12_STATIC_SAMPLER_DESC1& s) { return s; }

template <class Desc>
bool desc_from(const Desc& d, RootSignatureDesc& out, std::string& error) {
  out.flags = d.Flags;
  if ((d.NumParameters && !d.pParameters) || (d.NumStaticSamplers && !d.pStaticSamplers)) {
    error = "a null parameter or sampler array";
    return false;
  }
  for (UINT i = 0; i < d.NumParameters; ++i) {
    RootParameter p;
    if (!parameter_from(d.pParameters[i], p, error)) return false;
    out.parameters.push_back(std::move(p));
  }
  for (UINT i = 0; i < d.NumStaticSamplers; ++i) out.samplers.push_back(sampler_from(d.pStaticSamplers[i]));
  return true;
}

// ---- the serialized layout ----

class Writer {
 public:
  uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
  void u32(uint32_t v) { bytes(&v, 4); }
  void bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    bytes_.insert(bytes_.end(), b, b + n);
  }
  void patch(uint32_t at, uint32_t v) { std::memcpy(bytes_.data() + at, &v, 4); }
  std::vector<uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<uint8_t> bytes_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool u32(uint64_t at, uint32_t& v) const {
    if (at + 4 > size_) return false;
    std::memcpy(&v, data_ + at, 4);
    return true;
  }
  bool bytes(uint64_t at, void* out, size_t n) const {
    if (at + n > size_) return false;
    std::memcpy(out, data_ + at, n);
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
};

constexpr size_t kSampler10 = sizeof(D3D12_STATIC_SAMPLER_DESC);  // 52
constexpr size_t kSampler12 = sizeof(D3D12_STATIC_SAMPLER_DESC1);  // 56
static_assert(kSampler10 == 52 && kSampler12 == 56);

bool parse_raw(const uint8_t* data, size_t size, RootSignatureDesc& out) {
  const Reader r(data, size);
  uint32_t count = 0, params_at = 0, samplers = 0, samplers_at = 0;
  if (!r.u32(0, out.version) || !r.u32(4, count) || !r.u32(8, params_at) || !r.u32(12, samplers) ||
      !r.u32(16, samplers_at) || !r.u32(20, out.flags)) {
    return false;
  }
  if (out.version < 1 || out.version > 3 || count > 4096 || samplers > 4096) return false;
  const bool v11 = out.version >= 2;
  for (uint32_t i = 0; i < count; ++i) {
    RootParameter p;
    uint32_t payload = 0;
    const uint64_t at = params_at + uint64_t{i} * 12;
    if (!r.u32(at, p.type) || !r.u32(at + 4, p.visibility) || !r.u32(at + 8, payload)) return false;
    switch (p.type) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
        uint32_t ranges = 0, ranges_at = 0;
        if (!r.u32(payload, ranges) || !r.u32(payload + 4, ranges_at) || ranges > 65536) return false;
        const uint32_t stride = v11 ? 24 : 20;
        for (uint32_t k = 0; k < ranges; ++k) {
          const uint64_t q = ranges_at + uint64_t{k} * stride;
          RootRange g;
          if (!r.u32(q, g.type) || !r.u32(q + 4, g.count) || !r.u32(q + 8, g.base_register) || !r.u32(q + 12, g.space))
            return false;
          if (v11 ? !r.u32(q + 16, g.flags) || !r.u32(q + 20, g.table_offset) : !r.u32(q + 16, g.table_offset))
            return false;
          p.ranges.push_back(g);
        }
        break;
      }
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        if (!r.u32(payload, p.shader_register) || !r.u32(payload + 4, p.space) || !r.u32(payload + 8, p.constants))
          return false;
        break;
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        if (!r.u32(payload, p.shader_register) || !r.u32(payload + 4, p.space)) return false;
        if (v11 && !r.u32(payload + 8, p.flags)) return false;
        break;
      default: return false;
    }
    out.parameters.push_back(std::move(p));
  }
  const size_t stride = out.version >= 3 ? kSampler12 : kSampler10;
  for (uint32_t i = 0; i < samplers; ++i) {
    D3D12_STATIC_SAMPLER_DESC1 s{};
    if (!r.bytes(samplers_at + uint64_t{i} * stride, &s, stride)) return false;
    out.samplers.push_back(s);
  }
  return true;
}

// ---- root signature objects ----

struct RootSignature : Object {
  RootSignatureDesc desc;
  std::vector<uint8_t> bytes;  // as created from
};

HRESULT WINAPI root_signature_set_name(Object*, const wchar_t*) { return S_OK; }

const Kind& root_signature_kind() {
  static const Kind k("root signature", 16, {{6, "SetName", reinterpret_cast<void*>(&root_signature_set_name)}});
  return k;
}

Object* make_blob(std::vector<uint8_t> bytes) {
  auto* b = make_object<Blob>(blob_kind());
  b->bytes = std::move(bytes);
  return b;
}

}  // namespace

bool read_root_signature_desc(const void* versioned, RootSignatureDesc& out, std::string& error) {
  if (!versioned) {
    error = "no description";
    return false;
  }
  const auto* v = static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(versioned);
  out = {};
  out.version = v->Version;
  switch (v->Version) {
    case D3D_ROOT_SIGNATURE_VERSION_1_0: return desc_from(v->Desc_1_0, out, error);
    case D3D_ROOT_SIGNATURE_VERSION_1_1: return desc_from(v->Desc_1_1, out, error);
    case D3D_ROOT_SIGNATURE_VERSION_1_2: return desc_from(v->Desc_1_2, out, error);
    default: error = "an unknown root signature version " + std::to_string(v->Version); return false;
  }
}

std::vector<uint8_t> serialize_root_signature(const RootSignatureDesc& d) {
  const bool v11 = d.version >= 2;
  Writer w;
  for (int i = 0; i < 6; ++i) w.u32(0);  // the header, filled in below
  const uint32_t params_at = w.size();
  for (size_t i = 0; i < d.parameters.size(); ++i) {
    w.u32(d.parameters[i].type);
    w.u32(d.parameters[i].visibility);
    w.u32(0);  // payload offset
  }
  for (size_t i = 0; i < d.parameters.size(); ++i) {
    const RootParameter& p = d.parameters[i];
    w.patch(params_at + static_cast<uint32_t>(i) * 12 + 8, w.size());
    switch (p.type) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        w.u32(static_cast<uint32_t>(p.ranges.size()));
        w.u32(w.size() + 4);  // the ranges follow
        for (const RootRange& g : p.ranges) {
          w.u32(g.type);
          w.u32(g.count);
          w.u32(g.base_register);
          w.u32(g.space);
          if (v11) w.u32(g.flags);
          w.u32(g.table_offset);
        }
        break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        w.u32(p.shader_register);
        w.u32(p.space);
        w.u32(p.constants);
        break;
      default:
        w.u32(p.shader_register);
        w.u32(p.space);
        if (v11) w.u32(p.flags);
        break;
    }
  }
  const uint32_t samplers_at = w.size();
  for (const D3D12_STATIC_SAMPLER_DESC1& s : d.samplers) w.bytes(&s, d.version >= 3 ? kSampler12 : kSampler10);
  w.patch(0, d.version);
  w.patch(4, static_cast<uint32_t>(d.parameters.size()));
  w.patch(8, params_at);
  w.patch(12, static_cast<uint32_t>(d.samplers.size()));
  w.patch(16, samplers_at);
  w.patch(20, d.flags);
  return w.take();
}

bool parse_root_signature(const void* data, size_t size, RootSignatureDesc& out) {
  out = {};
  const auto* b = static_cast<const uint8_t*>(data);
  if (!b || size < 24) return false;
  // A shader container: find its RTS0 part.
  if (std::memcmp(b, "DXBC", 4) == 0) {
    const Reader r(b, size);
    uint32_t parts = 0;
    if (!r.u32(28, parts) || parts > 64) return false;
    for (uint32_t i = 0; i < parts; ++i) {
      uint32_t at = 0, fourcc = 0, part_size = 0;
      if (!r.u32(32 + uint64_t{i} * 4, at) || !r.u32(at, fourcc) || !r.u32(at + 4, part_size)) return false;
      if (std::memcmp(&fourcc, "RTS0", 4) == 0) {
        if (uint64_t{at} + 8 + part_size > size) return false;
        return parse_raw(b + at + 8, part_size, out);
      }
    }
    return false;
  }
  return parse_raw(b, size, out);
}

HRESULT WINAPI serialize_versioned_root_signature(const void* desc, void** blob, void** error_blob) {
  if (error_blob) *error_blob = nullptr;
  if (!blob) return E_POINTER;
  *blob = nullptr;
  RootSignatureDesc d;
  std::string error;
  if (!read_root_signature_desc(desc, d, error)) {
    logf("d3d12x: D3D12SerializeVersionedRootSignature: %s", error.c_str());
    if (error_blob) {
      std::vector<uint8_t> text(error.begin(), error.end());
      text.push_back(0);
      *error_blob = make_blob(std::move(text));
    }
    return E_INVALIDARG;
  }
  std::vector<uint8_t> bytes = serialize_root_signature(d);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
    logf("d3d12x: D3D12SerializeVersionedRootSignature(version %u, %zu parameters, %zu samplers, flags 0x%x) = %zu bytes",
         d.version, d.parameters.size(), d.samplers.size(), d.flags, bytes.size());
  }
  *blob = make_blob(std::move(bytes));
  return S_OK;
}

const RootSignatureDesc* root_signature_desc(const void* object) {
  Object* o = object_of_kind(object, root_signature_kind());
  return o ? &static_cast<RootSignature*>(o)->desc : nullptr;
}

void user_data_sgprs(const RootSignatureDesc& desc, uint32_t visibility, const uint64_t* args,
                     const std::vector<uint32_t>* constants, uint32_t* sgprs, uint32_t count) {
  uint32_t at = 8;
  for (size_t k = 0; k < desc.parameters.size() && at < count; ++k) {
    const RootParameter& p = desc.parameters[k];
    if (p.visibility != D3D12_SHADER_VISIBILITY_ALL && p.visibility != visibility) continue;
    switch (p.type) {
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        sgprs[at++] = static_cast<uint32_t>(args[k]);
        break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        for (uint32_t c = 0; c < p.constants && at < count; ++c) {
          sgprs[at++] = c < constants[k].size() ? constants[k][c] : 0u;
        }
        break;
      default:  // root CBV / SRV / UAV
        sgprs[at++] = static_cast<uint32_t>(args[k]);
        if (at < count) sgprs[at++] = static_cast<uint32_t>(args[k] >> 32);
        break;
    }
  }
}

HRESULT create_root_signature(uint32_t node_mask, const void* data, size_t size, const GUID* /*riid*/, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  RootSignatureDesc d;
  if (!parse_root_signature(data, size, d)) {
    logf("d3d12x: CreateRootSignature(%zu bytes): not a root signature we can read", size);
    return E_INVALIDARG;
  }
  auto* rs = make_object<RootSignature>(root_signature_kind());
  rs->desc = std::move(d);
  rs->bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
    logf("d3d12x: CreateRootSignature(node mask %u, %zu bytes: version %u, %zu parameters, %zu samplers, flags 0x%x) = %p",
         node_mask, size, rs->desc.version, rs->desc.parameters.size(), rs->desc.samplers.size(), rs->desc.flags,
         static_cast<void*>(rs));
  }
  *out = rs;
  return S_OK;
}

}  // namespace am::rt::d3d12x
