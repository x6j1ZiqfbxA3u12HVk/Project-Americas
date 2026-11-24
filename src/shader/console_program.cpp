#include "shader/console_program.h"

#include <cstring>

#include "formats/inflate.h"

namespace am::shader {
namespace {

constexpr size_t kXdatAt = 0x18;
constexpr uint32_t kMaxEntries = 64;
constexpr uint32_t kKindNative = 0x0d;
constexpr uint32_t kXbchVersion = 3;
constexpr size_t kXbchHeader = 0x18;
constexpr uint32_t kFssmVersion = 0x101;
constexpr size_t kFssmHeader = 0x10;
constexpr uint64_t kAbsent = 0xDEADBEEFDEADBEEFull;
constexpr uint64_t kMaxUnpacked = 16u << 20;
constexpr uint64_t kRecordSize = 0x54;

struct Slot {
  HwStage stage;
  size_t at;
};
constexpr Slot kSlots[] = {{HwStage::Ls, 0x08}, {HwStage::Hs, 0x10}, {HwStage::Es, 0x18}, {HwStage::Gs, 0x20},
                           {HwStage::Vs, 0x28}, {HwStage::Ps, 0x30}, {HwStage::Ms, 0x48}, {HwStage::Cs, 0x60}};

template <class T>
bool read(std::span<const uint8_t> d, uint64_t at, T& out) {
  if (at > d.size() || d.size() - at < sizeof(T)) return false;
  std::memcpy(&out, d.data() + at, sizeof(T));
  return true;
}

bool tag(std::span<const uint8_t> d, uint64_t at, const char* t) { return at + 4 <= d.size() && std::memcmp(d.data() + at, t, 4) == 0; }

bool read_blob(std::span<const uint8_t> blob, NativeBlob& out, std::string& error) {
  uint32_t version = 0;
  uint64_t packed = 0, unpacked = 0;
  if (!tag(blob, 0, "XBCH") || !read(blob, 4, version) || !read(blob, 8, packed) || !read(blob, 16, unpacked)) {
    error = "native blob lacks its XBCH header";
    return false;
  }
  if (version != kXbchVersion) {
    error = "unknown XBCH version " + std::to_string(version);
    return false;
  }
  if (unpacked > kMaxUnpacked || packed > blob.size() - kXbchHeader) {
    error = "implausible XBCH sizes";
    return false;
  }
  std::vector<uint8_t> raw(static_cast<size_t>(unpacked));
  const auto r = formats::inflate_zlib(blob.subspan(kXbchHeader, static_cast<size_t>(packed)), raw);
  if (r.status != formats::InflateStatus::Ok || r.written != unpacked) {
    error = std::string("XBCH stream: ") + formats::inflate_status_name(r.status);
    return false;
  }
  uint32_t fssm_version = 0;
  uint64_t size = 0;
  if (!tag(raw, 0, "FSSM") || !read(std::span<const uint8_t>(raw), 4, fssm_version) || !read(std::span<const uint8_t>(raw), 8, size)) {
    error = "unpacked blob lacks its FSSM header";
    return false;
  }
  if (fssm_version != kFssmVersion) {
    error = "unknown FSSM version " + std::to_string(fssm_version);
    return false;
  }
  if (size > raw.size() - kFssmHeader) {
    error = "FSSM payload out of bounds";
    return false;
  }
  out.payload.assign(raw.begin() + kFssmHeader, raw.begin() + kFssmHeader + static_cast<ptrdiff_t>(size));
  const std::span<const uint8_t> p(out.payload);
  for (const Slot& s : kSlots) {
    uint64_t record = 0;
    if (!read(p, s.at, record)) {
      error = "FSSM slot table truncated";
      return false;
    }
    if (record == kAbsent) continue;
    NativeProgram prog;
    prog.stage = s.stage;
    prog.record_offset = record;
    uint32_t regs = 0;
    if (record > p.size() || p.size() - record < kRecordSize || !read(p, record, prog.code_offset) ||
        !read(p, record + 8, prog.code_size) || !read(p, record + 0x14, prog.flags) ||
        !read(p, record + 0x4c, prog.lds_bytes) || !read(p, record + 0x50, regs)) {
      error = std::string(hw_stage_name(s.stage)) + " record out of bounds";
      return false;
    }
    if (prog.code_offset > p.size() || p.size() - prog.code_offset < prog.code_size || prog.code_size % 4) {
      error = std::string(hw_stage_name(s.stage)) + " code out of bounds";
      return false;
    }
    if (s.stage == HwStage::Cs) {
      const uint64_t e = record + kRecordSize;
      if (!read(p, e + 0x24, prog.cs_rsrc2) || !read(p, e + 0x28, prog.cs_threads[0]) ||
          !read(p, e + 0x2c, prog.cs_threads[1]) || !read(p, e + 0x30, prog.cs_threads[2])) {
        error = "CS stage data out of bounds";
        return false;
      }
    }
    prog.wave32 = (prog.flags >> 5) & 1;
    prog.vgprs = regs & 0x3FF;
    prog.sgprs = (regs >> 20) & 0xFF;
    out.programs.push_back(prog);
  }
  return true;
}

}  // namespace

const char* hw_stage_name(HwStage s) {
  static const char* const names[] = {"LS", "HS", "ES", "GS", "VS", "PS", "MS", "CS"};
  return names[static_cast<int>(s)];
}

std::optional<ConsoleShader> read_console_shader(std::span<const uint8_t> part, std::string& error) {
  uint32_t count = 0, entry_size = 0;
  if (!tag(part, kXdatAt, "XDAT") || !read(part, kXdatAt + 4, count) || !read(part, kXdatAt + 8, entry_size)) {
    error = "no XDAT index at +0x18";
    return std::nullopt;
  }
  if (count > kMaxEntries) {
    error = "implausible XDAT entry count";
    return std::nullopt;
  }
  ConsoleShader shader;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t kind = 0, offset = 0, size = 0;
    const size_t at = kXdatAt + 12 + 12 * size_t{i};
    if (!read(part, at, kind) || !read(part, at + 4, offset) || !read(part, at + 8, size)) {
      error = "XDAT index truncated";
      return std::nullopt;
    }
    if (kind != kKindNative) continue;
    if (offset > part.size() || part.size() - offset < size) {
      error = "XDAT entry out of bounds";
      return std::nullopt;
    }
    NativeBlob blob;
    if (!read_blob(part.subspan(offset, size), blob, error)) return std::nullopt;
    shader.blobs.push_back(std::move(blob));
  }
  if (shader.blobs.empty()) {
    error = "no native program blob";
    return std::nullopt;
  }
  return shader;
}

}  // namespace am::shader
