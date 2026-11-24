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

// Device slot 16, CreateRootSignature(node mask, blob, size, riid, out).
HRESULT create_root_signature(uint32_t node_mask, const void* data, size_t size, const GUID* riid, void** out);

}  // namespace am::rt::d3d12x
