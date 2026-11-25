#pragma once

// Root signatures: d3d12_xs!D3D12SerializeVersionedRootSignature, and the description the device
// reads back from a serialized blob in CreateRootSignature.
//
// Blobs use D3D12's serialized layout (the payload of a shader container's RTS0 part), so a root
// signature serialized here and one embedded in a shader read the same way:
//
//   header      version (1 = 1.0, 2 = 1.1, 3 = 1.2), parameter count, parameters' offset,
//               static sampler count, samplers' offset, flags
//   parameters  type, shader visibility, payload offset (12 bytes each)
//   payloads    tables: range count, ranges' offset; ranges of 20 bytes (1.0) or 24 (1.1, with
//               flags before the table offset); constants: register, space, count; descriptors:
//               register, space (and flags from 1.1)
//   samplers    D3D12's static sampler fields, 52 bytes (56 from 1.2, with flags)
//
// Offsets count from the blob's start. The console's own flag bits pass through unchanged.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace am::rt::d3d12x {

struct RootRange {
  uint32_t type = 0;  // D3D12_DESCRIPTOR_RANGE_TYPE
  uint32_t count = 0;
  uint32_t base_register = 0;
  uint32_t space = 0;
  uint32_t flags = 0;  // from 1.1
  uint32_t table_offset = 0;
};

struct RootParameter {
  uint32_t type = 0;  // D3D12_ROOT_PARAMETER_TYPE
  uint32_t visibility = 0;
  uint32_t shader_register = 0;  // constants and descriptors
  uint32_t space = 0;
  uint32_t constants = 0;  // 32-bit values, for root constants
  uint32_t flags = 0;      // descriptors, from 1.1
  std::vector<RootRange> ranges;  // descriptor tables
};

struct RootSignatureDesc {
  uint32_t version = 0;  // 1, 2 or 3
  uint32_t flags = 0;
  std::vector<RootParameter> parameters;
  std::vector<D3D12_STATIC_SAMPLER_DESC1> samplers;  // Flags is 0 before 1.2
};

// From a D3D12_VERSIONED_ROOT_SIGNATURE_DESC (any of 1.0, 1.1, 1.2).
bool read_root_signature_desc(const void* versioned, RootSignatureDesc& out, std::string& error);
std::vector<uint8_t> serialize_root_signature(const RootSignatureDesc& desc);
// A serialized blob, or a shader container holding one in its RTS0 part.
bool parse_root_signature(const void* data, size_t size, RootSignatureDesc& out);

// d3d12_xs!D3D12SerializeVersionedRootSignature(description, blob, error blob).
HRESULT WINAPI serialize_versioned_root_signature(const void* desc, void** blob, void** error_blob);

// The description of a root signature object of ours; nullptr for anything else.
const RootSignatureDesc* root_signature_desc(const void* object);

// The console's user data layout: each stage's root parameters (those visible to it, in order)
// fill its user data SGPRs from 8 on: a descriptor table 1 (the low half of its GPU address),
// a root CBV / SRV / UAV 2 (its address), root constants one each. Learned from the game's
// shaders: their descriptor tables and root views are read from s8 (and s9), and a pixel
// shader's primitive mask follows its last one. `visibility` is a D3D12_SHADER_VISIBILITY.
// `args` holds each parameter's address, `constants` each parameter's 32-bit values.
void user_data_sgprs(const RootSignatureDesc& desc, uint32_t visibility, const uint64_t* args,
                     const std::vector<uint32_t>* constants, uint32_t* sgprs, uint32_t count);

// Device slot 16, CreateRootSignature(node mask, blob, size, riid, out).
HRESULT create_root_signature(uint32_t node_mask, const void* data, size_t size, const GUID* riid, void** out);

}  // namespace am::rt::d3d12x
