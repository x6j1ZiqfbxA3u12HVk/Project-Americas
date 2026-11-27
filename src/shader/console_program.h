#pragma once

// The console's native GPU programs inside a container's DXIL part. Layout, checked on all
// 25,889 containers of the game's shader pack:
//
//   DXIL part +0x18  "XDAT", u32 entry count, u32 0x24, then entries of u32 kind, offset, size
//                    (offsets from the part's start). Kind 0x0d is a native program blob; the
//                    others seen are 7 (the PC program header and a dummy bitcode) and 0x12.
//   blob             "XBCH", u32 version 3, u64 packed size, u64 unpacked size, a zlib stream
//   unpacked         "FSSM", u32 version 0x101, u64 payload size, the payload
//   payload +0x00    u64 count of graphics stage slots (6)
//           +0x08..  u64 record offsets: LS HS ES GS VS PS at 0x08..0x30, mesh at 0x48, compute
//                    at 0x60; 0xDEADBEEFDEADBEEF where a stage is absent
//   record  +0x00    u64 code offset, +0x08 u64 code size (from the payload's start)
//           +0x14    u32 flags: bit 5 wave32 (else wave64)
//           +0x4c    u32 LDS bytes; +0x50 u32 registers: bits 0-9 VGPRs, 20-27 SGPRs
//   stage data follows the record (inputs, outputs, export patches; read by the translator).
//   Compute: +0x54+0x24 u32 COMPUTE_PGM_RSRC2, +0x54+0x28 u32 x3 thread-group size.
//   Mesh: +0x54+0xc8 outputs as a vertex shader's (param bit 7: per primitive), +0x54+0x18c
//   eight registers (SPI_SHADER_PGM_RSRC2_GS the fifth), +0x54+0x448 u16 lanes, u16 most
//   vertices, u16 most primitives, u8 user SGPRs, u8 topology.
//
// A vertex shader can carry two blobs (for use with and without a geometry shader).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace am::shader {

enum class HwStage : uint8_t { Ls, Hs, Es, Gs, Vs, Ps, Ms, Cs };
const char* hw_stage_name(HwStage s);

struct NativeProgram {
  HwStage stage = HwStage::Vs;
  uint64_t record_offset = 0;  // in the payload
  uint64_t code_offset = 0;
  uint64_t code_size = 0;
  uint32_t flags = 0;
  bool wave32 = false;
  uint32_t vgprs = 0, sgprs = 0;  // the record's raw counts
  uint32_t lds_bytes = 0;
  // Compute only: COMPUTE_PGM_RSRC2 (user SGPRs, group id SGPRs, thread id VGPRs) and the
  // thread-group size, from the stage data after the record (record + 0x54 + 0x24 / + 0x28).
  uint32_t cs_rsrc2 = 0;
  uint32_t cs_threads[3] = {0, 0, 0};
  // Mesh only (stage data E + 0x18c + 16, E + 0x448): SPI_SHADER_PGM_RSRC2_GS, the group's
  // lanes, the most vertices and primitives it outputs, its user SGPRs, its topology (2
  // triangles). Lane n exports vertex n and primitive n.
  uint32_t ms_rsrc2 = 0, ms_threads = 0, ms_max_vertices = 0, ms_max_primitives = 0, ms_user_sgprs = 0, ms_topology = 0;
};

struct NativeBlob {
  std::vector<uint8_t> payload;  // the FSSM payload
  std::vector<NativeProgram> programs;
  std::span<const uint8_t> code(const NativeProgram& p) const {
    return std::span<const uint8_t>(payload).subspan(p.code_offset, p.code_size);
  }
  std::span<const uint8_t> record(const NativeProgram& p) const {
    return std::span<const uint8_t>(payload).subspan(p.record_offset);
  }
};

struct ConsoleShader {
  std::vector<NativeBlob> blobs;
};

// From a DXIL part's data. Unknown versions are refused, so a format change shows as an error.
std::optional<ConsoleShader> read_console_shader(std::span<const uint8_t> dxil_part, std::string& error);

}  // namespace am::shader
