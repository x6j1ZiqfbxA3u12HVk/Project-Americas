// The shader pack scanner on a synthetic pack: junk, a decoy framed like a record whose data does
// not inflate, and two real records (stored DEFLATE blocks holding minimal DXBC containers).

#include <cstring>
#include <string>
#include <vector>

#include "shader/container.h"
#include "shader/pack.h"
#include "test.h"

using namespace am::shader;

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// A DXBC container with one part.
std::vector<uint8_t> container(const char* fourcc, uint32_t payload_size) {
  std::vector<uint8_t> c = {'D', 'X', 'B', 'C'};
  c.resize(20, 0);  // hash
  put32(c, 1);      // version
  const uint32_t total = 32 + 4 + 8 + payload_size;
  put32(c, total);
  put32(c, 1);   // one part
  put32(c, 36);  // its offset
  c.insert(c.end(), fourcc, fourcc + 4);
  put32(c, payload_size);
  for (uint32_t i = 0; i < payload_size; ++i) c.push_back(static_cast<uint8_t>(i));
  return c;
}

// One record: name, header, and the container as a stored DEFLATE block.
void append_record(std::vector<uint8_t>& pack, const std::string& name, const std::vector<uint8_t>& data,
                   bool corrupt = false) {
  std::vector<uint8_t> deflate = {0x01, static_cast<uint8_t>(data.size()), static_cast<uint8_t>(data.size() >> 8),
                                  static_cast<uint8_t>(~data.size()), static_cast<uint8_t>(~data.size() >> 8)};
  deflate.insert(deflate.end(), data.begin(), data.end());
  if (corrupt) deflate[3] ^= 0xFF;  // LEN / NLEN no longer match
  pack.push_back(static_cast<uint8_t>(name.size() + 1));
  pack.insert(pack.end(), name.begin(), name.end());
  pack.push_back(0);
  pack.push_back(0x32);  // flags
  put32(pack, static_cast<uint32_t>(deflate.size()));
  put32(pack, 0);
  put32(pack, static_cast<uint32_t>(deflate.size()));
  put32(pack, static_cast<uint32_t>(data.size()));
  pack.insert(pack.end(), deflate.begin(), deflate.end());
}

}  // namespace

TEST_CASE(shader_pack_scan_finds_records_and_skips_decoys) {
  std::vector<uint8_t> pack = {'9', 'F', 'P', 'R', 0x11, 0x22, 0x33, 0x00, 0x05};
  append_record(pack, "PS_Decoy_Wrapped", container("TEST", 16), /*corrupt=*/true);
  pack.insert(pack.end(), {0x00, 0x7F, 0x41, 0x42});
  // The name's own bytes include one equal to the distance to its terminator ('\x06' + "ab_Wrapped" has no such
  // byte, so use a name whose tail could look like a record: "X:" + 9 chars after ':').
  append_record(pack, "fx:VS_First_Wrapped", container("ISG1", 24));
  append_record(pack, "CS_Second_Wrapped", container("PSV0", 40));
  pack.insert(pack.end(), 64, 0xCC);

  std::vector<std::string> names;
  std::vector<size_t> sizes;
  PackScanStats stats;
  scan_pack(
      pack,
      [&](const PackRecord& r, std::span<const uint8_t> c) {
        names.push_back(r.name);
        sizes.push_back(c.size());
      },
      stats);
  CHECK(names.size() == 2);
  CHECK(names.size() == 2 && names[0] == "fx:VS_First_Wrapped" && names[1] == "CS_Second_Wrapped");
  CHECK(sizes.size() == 2 && sizes[0] == 32 + 12 + 24 && sizes[1] == 32 + 12 + 40);
  CHECK(stats.records == 2);
  CHECK(stats.rejected == 1);  // the decoy
}

TEST_CASE(shader_container_parts) {
  const std::vector<uint8_t> c = container("PSV0", 40);
  std::string error;
  const auto parsed = parse_container(c, error);
  CHECK(parsed.has_value());
  if (!parsed) return;
  CHECK(parsed->parts.size() == 1);
  CHECK(parsed->find("PSV0") != nullptr && parsed->find("PSV0")->data.size() == 40);
  CHECK(parsed->find("DXIL") == nullptr);
  std::vector<uint8_t> broken = c;
  broken[24] = 0xFF;  // total size beyond the blob
  CHECK(!parse_container(broken, error).has_value());
}
