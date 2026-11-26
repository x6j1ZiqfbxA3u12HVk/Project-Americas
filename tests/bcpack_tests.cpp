#include <cstdint>
#include <string>
#include <vector>

#include "formats/bcpack.h"
#include "test.h"

using namespace am::formats::bcpack;

namespace {

// A table file of our own: every model uniform over its buckets (model 16 g + shape uses g + 2
// buckets; bucket b > 0 holds 2^(b-1) symbols, so a frequency of 2^(15 - g) each tiles 65536).
std::vector<uint8_t> uniform_tables() {
  std::vector<uint8_t> t;
  for (int m = 0; m < 128; ++m) {
    const int g = m / 16;
    for (int b = 0; b < 9; ++b) {
      const uint16_t f = b < g + 2 ? static_cast<uint16_t>(1u << (15 - g)) : 0;
      t.push_back(static_cast<uint8_t>(f));
      t.push_back(static_cast<uint8_t>(f >> 8));
    }
  }
  t.resize(table_file_size(), 0);
  return t;
}

}  // namespace

TEST_CASE(bcpack_tables_need_the_right_size) {
  std::string error;
  std::vector<uint8_t> bytes(100, 0);
  CHECK(Tables::load(bytes, error) == nullptr);
  CHECK(!error.empty());
}

TEST_CASE(bcpack_tables_need_models_that_tile_65536) {
  std::vector<uint8_t> bytes = uniform_tables();
  std::string error;
  CHECK(Tables::load(bytes, error) != nullptr);
  bytes[0] ^= 1;  // model 0's first frequency: the model no longer tiles
  CHECK(Tables::load(bytes, error) == nullptr);
}

TEST_CASE(bcpack_refuses_what_it_cannot_decode) {
  std::string error;
  const auto tables = Tables::load(uniform_tables(), error);
  CHECK(tables != nullptr);
  std::vector<uint8_t> out(16);
  const std::vector<uint8_t> src(64, 0);
  CHECK(decode(2, src, out, *tables) == Status::Unsupported);  // BC2: not yet
  std::vector<uint8_t> odd(12);
  CHECK(decode(1, src, odd, *tables) == Status::BadSize);  // BC1 blocks are 8 bytes
  CHECK(decode(1, {}, out, *tables) == Status::Corrupt);   // no input at all
}
