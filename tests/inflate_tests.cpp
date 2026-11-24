// DEFLATE vectors made with zlib from synthetic text: a stored block, fixed codes, fixed codes
// with back-references, a dynamic (Huffman-only) block, and the zlib wrapper.

#include <string>
#include <vector>

#include "formats/inflate.h"
#include "test.h"

using namespace am::formats;

namespace {

struct Out {
  InflateResult result;
  std::string text;
};

Out run_raw(std::vector<uint8_t> in, size_t capacity = 4096) {
  std::vector<uint8_t> out(capacity);
  Out o;
  o.result = inflate_raw(in, out);
  o.text.assign(reinterpret_cast<const char*>(out.data()), o.result.written);
  return o;
}

std::string repeat(const std::string& s, int n) {
  std::string r;
  while (n--) r += s;
  return r;
}

const std::vector<uint8_t> kHuffmanOnly = {0x05, 0xc1, 0xb1, 0x09, 0x00, 0x00, 0x08, 0x04, 0xb1, 0x55,
                                           0x5c, 0xce, 0xe2, 0x0a, 0xe1, 0xf1, 0xc0, 0xf9, 0x4d, 0x06,
                                           0x45, 0x49, 0xa8, 0xe5, 0x7a, 0x6b, 0x50, 0x94, 0x84, 0x07};

}  // namespace

TEST_CASE(inflate_stored_block) {
  const Out o = run_raw({0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'});
  CHECK(o.result.status == InflateStatus::Ok);
  CHECK(o.text == "hello");
  CHECK(o.result.consumed == 10);
}

TEST_CASE(inflate_fixed_codes) {
  const Out o = run_raw({0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00});
  CHECK(o.result.status == InflateStatus::Ok);
  CHECK(o.text == "hello");
  CHECK(o.result.consumed == 7);
}

TEST_CASE(inflate_fixed_codes_with_matches) {
  const Out o = run_raw({0x4b, 0x4c, 0x84, 0x81, 0x24, 0x28, 0x48, 0x86, 0x80, 0x14, 0x10,
                         0x48, 0x05, 0x82, 0xb4, 0xb4, 0xf4, 0x44, 0x62, 0x14, 0x01, 0x00});
  CHECK(o.result.status == InflateStatus::Ok);
  CHECK(o.text == repeat("aaaaaaaaaabbbbbbbbcccccccdddddeeeeffg", 2));
}

TEST_CASE(inflate_dynamic_block) {
  const Out o = run_raw(kHuffmanOnly);
  CHECK(o.result.status == InflateStatus::Ok);
  CHECK(o.text == "mississippi river mississippi");
  CHECK(o.result.consumed == kHuffmanOnly.size());
}

TEST_CASE(inflate_reports_truncation_overflow_and_corruption) {
  CHECK(run_raw(std::vector<uint8_t>(kHuffmanOnly.begin(), kHuffmanOnly.begin() + 15)).result.status ==
        InflateStatus::Truncated);
  CHECK(run_raw({0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00}, 3).result.status == InflateStatus::OutputFull);
  CHECK(run_raw({0x07}).result.status == InflateStatus::Corrupt);                    // block type 3
  CHECK(run_raw({0x01, 0x05, 0x00, 0x00, 0x00}).result.status == InflateStatus::Corrupt);  // LEN / NLEN mismatch
  CHECK(run_raw({}).result.status == InflateStatus::Truncated);
}

TEST_CASE(inflate_zlib_stream_and_adler32) {
  const std::vector<uint8_t> z = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15};
  std::vector<uint8_t> out(64);
  const InflateResult r = inflate_zlib(z, out);
  CHECK(r.status == InflateStatus::Ok);
  CHECK(r.written == 5 && std::string(reinterpret_cast<char*>(out.data()), 5) == "hello");
  CHECK(r.consumed == z.size());
  std::vector<uint8_t> bad = z;
  bad.back() ^= 1;
  CHECK(inflate_zlib(bad, out).status == InflateStatus::Corrupt);
  const std::string hello = "hello";
  CHECK(adler32({reinterpret_cast<const uint8_t*>(hello.data()), hello.size()}) == 0x062C0215u);
}
