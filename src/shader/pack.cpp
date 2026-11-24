#include "shader/pack.h"

#include <cstring>
#include <vector>

#include "formats/inflate.h"

namespace am::shader {
namespace {

constexpr uint32_t kMaxContainer = 64u << 20;

bool name_char(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '.' ||
         c == '-';
}

uint32_t u32_at(std::span<const uint8_t> b, size_t o) {
  uint32_t v;
  std::memcpy(&v, b.data() + o, 4);
  return v;
}

}  // namespace

void scan_pack(std::span<const uint8_t> pack,
               const std::function<void(const PackRecord&, std::span<const uint8_t> container)>& on_record,
               PackScanStats& stats) {
  std::vector<uint8_t> buffer;
  const size_t size = pack.size();
  size_t p = 0;
  while (p + 2 < size) {
    const size_t n = pack[p];
    // name (n - 1 bytes), its terminator, then 17 header bytes
    if (n < 2 || p + n + 18 > size || pack[p + n] != 0) {
      ++p;
      continue;
    }
    bool ok = true;
    for (size_t i = 1; i < n && ok; ++i) ok = name_char(pack[p + i]);
    if (!ok) {
      ++p;
      continue;
    }
    const size_t h = p + n + 1;
    const uint32_t packed = u32_at(pack, h + 1);
    const uint32_t packed_again = u32_at(pack, h + 9);
    const uint32_t unpacked = u32_at(pack, h + 13);
    const size_t data = h + 17;
    if (packed == 0 || packed != packed_again || unpacked < 32 || unpacked > kMaxContainer || data + packed > size) {
      ++p;
      continue;
    }
    buffer.resize(unpacked);
    const formats::InflateResult r = formats::inflate_raw(pack.subspan(data, packed), buffer);
    if (r.status != formats::InflateStatus::Ok || r.written != unpacked || std::memcmp(buffer.data(), "DXBC", 4) != 0) {
      ++stats.rejected;
      ++p;
      continue;
    }
    PackRecord rec;
    rec.name.assign(reinterpret_cast<const char*>(pack.data() + p + 1), n - 1);
    rec.offset = p;
    rec.flags = pack[h];
    rec.unknown = u32_at(pack, h + 5);
    rec.data = data;
    rec.packed_size = packed;
    rec.size = unpacked;
    ++stats.records;
    on_record(rec, buffer);
    p = data + packed;  // past the payload: a suffix of this name must not count as another record
  }
}

}  // namespace am::shader
