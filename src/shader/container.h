#pragma once

// DXBC shader containers: a header, then parts tagged with four-character codes. The game's
// containers have the PC's signature and validation parts (ISG1, OSG1, PSV0...) plus a DXIL part
// whose program header is the PC's, but whose payload is console data ("XDAT...") instead of
// LLVM bitcode.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace am::shader {

struct Part {
  std::string fourcc;
  uint32_t offset = 0;  // of the part's data in the container
  std::span<const uint8_t> data;
};

struct Container {
  uint32_t total_size = 0;
  std::vector<Part> parts;
  const Part* find(std::string_view fourcc) const;
};

std::optional<Container> parse_container(std::span<const uint8_t> blob, std::string& error);

enum class ProgramKind : uint8_t {
  Pixel = 0,
  Vertex = 1,
  Geometry = 2,
  Hull = 3,
  Domain = 4,
  Compute = 5,
  Library = 6,
  Mesh = 13,
  Amplification = 14,
};
const char* program_kind_name(uint32_t kind);

// The header at the start of a DXIL part.
struct ProgramHeader {
  uint32_t version = 0;      // (kind << 16) | (major << 4) | minor
  uint32_t size_dwords = 0;  // the part's size
  uint32_t magic = 0;        // "DXIL"
  uint32_t dxil_version = 0;
  uint32_t payload_offset = 0;  // from the magic
  uint32_t payload_size = 0;
  uint32_t kind() const { return version >> 16; }
  uint32_t major() const { return (version >> 4) & 0xF; }
  uint32_t minor() const { return version & 0xF; }
};

std::optional<ProgramHeader> read_program_header(std::span<const uint8_t> dxil_part);

// The four-character code at `p`, printable characters only ("" otherwise).
std::string fourcc_at(std::span<const uint8_t> data, size_t p);

}  // namespace am::shader
