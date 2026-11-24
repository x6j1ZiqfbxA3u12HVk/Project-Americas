#include "shader/container.h"

#include <cstring>

namespace am::shader {
namespace {

uint32_t u32_at(std::span<const uint8_t> b, size_t o) {
  uint32_t v;
  std::memcpy(&v, b.data() + o, 4);
  return v;
}

}  // namespace

const Part* Container::find(std::string_view fourcc) const {
  for (const Part& p : parts) {
    if (p.fourcc == fourcc) return &p;
  }
  return nullptr;
}

std::string fourcc_at(std::span<const uint8_t> data, size_t p) {
  if (p + 4 > data.size()) return {};
  std::string s(reinterpret_cast<const char*>(data.data() + p), 4);
  for (char c : s) {
    if (c < 0x20 || c > 0x7E) return {};
  }
  return s;
}

std::optional<Container> parse_container(std::span<const uint8_t> blob, std::string& error) {
  // "DXBC", 16-byte hash, u32 version (1), u32 total size, u32 part count, u32 offsets[count]
  if (blob.size() < 32 || std::memcmp(blob.data(), "DXBC", 4) != 0) {
    error = "not a DXBC container";
    return std::nullopt;
  }
  Container c;
  c.total_size = u32_at(blob, 24);
  const uint32_t count = u32_at(blob, 28);
  if (c.total_size > blob.size() || count > 64 || 32 + uint64_t{count} * 4 > blob.size()) {
    error = "container header out of range";
    return std::nullopt;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t off = u32_at(blob, 32 + i * 4);
    if (uint64_t{off} + 8 > c.total_size) {
      error = "part " + std::to_string(i) + " out of range";
      return std::nullopt;
    }
    const uint32_t size = u32_at(blob, off + 4);
    if (uint64_t{off} + 8 + size > c.total_size) {
      error = "part " + std::to_string(i) + " runs past the container";
      return std::nullopt;
    }
    Part p;
    p.fourcc = fourcc_at(blob, off);
    if (p.fourcc.empty()) p.fourcc = "????";
    p.offset = off + 8;
    p.data = blob.subspan(off + 8, size);
    c.parts.push_back(std::move(p));
  }
  return c;
}

const char* program_kind_name(uint32_t kind) {
  switch (kind) {
    case 0: return "pixel";
    case 1: return "vertex";
    case 2: return "geometry";
    case 3: return "hull";
    case 4: return "domain";
    case 5: return "compute";
    case 6: return "library";
    case 7: return "ray generation";
    case 8: return "intersection";
    case 9: return "any hit";
    case 10: return "closest hit";
    case 11: return "miss";
    case 12: return "callable";
    case 13: return "mesh";
    case 14: return "amplification";
    default: return "other";
  }
}

std::optional<ProgramHeader> read_program_header(std::span<const uint8_t> part) {
  if (part.size() < 24) return std::nullopt;
  ProgramHeader h;
  h.version = u32_at(part, 0);
  h.size_dwords = u32_at(part, 4);
  h.magic = u32_at(part, 8);
  h.dxil_version = u32_at(part, 12);
  h.payload_offset = u32_at(part, 16);
  h.payload_size = u32_at(part, 20);
  if (std::memcmp(part.data() + 8, "DXIL", 4) != 0) return std::nullopt;
  return h;
}

}  // namespace am::shader
