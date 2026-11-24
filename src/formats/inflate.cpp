#include "formats/inflate.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace am::formats {
namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10;

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// LSB-first bit reader over the input. Past the end it supplies zero bytes and counts them, so a
// truncated stream is detected after the fact instead of by checks on every read.
class Bits {
 public:
  explicit Bits(std::span<const uint8_t> in) : p_(in.data()), end_(in.data() + in.size()), start_(in.data()) {}

  void refill() {
    while (count_ <= 56) {
      if (p_ < end_) {
        buf_ |= uint64_t{*p_++} << count_;
      } else {
        ++phantom_;
      }
      count_ += 8;
    }
  }
  uint32_t peek(int n) {
    if (count_ < n) refill();
    return static_cast<uint32_t>(buf_ & ((uint64_t{1} << n) - 1));
  }
  void drop(int n) {
    buf_ >>= n;
    count_ -= n;
  }
  uint32_t take(int n) {
    if (n == 0) return 0;
    const uint32_t v = peek(n);
    drop(n);
    return v;
  }
  void align_to_byte() { drop(count_ & 7); }
  // Input fully consumed so far, in bytes (bits still buffered are not consumed).
  size_t consumed() const { return static_cast<size_t>(p_ - start_) - (count_ / 8 > phantom_ ? count_ / 8 - phantom_ : 0); }
  bool overrun() const {
    // Phantom bytes that were actually taken (not merely buffered) mean the input ran out.
    return phantom_ * 8 > static_cast<size_t>(count_);
  }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
  const uint8_t* start_;
  uint64_t buf_ = 0;
  int count_ = 0;
  size_t phantom_ = 0;
};

uint32_t reverse_bits(uint32_t code, int length) {
  uint32_t r = 0;
  for (int i = 0; i < length; ++i) {
    r = (r << 1) | (code & 1);
    code >>= 1;
  }
  return r;
}

// A canonical Huffman code: a lookup table for codes up to kFastBits long, and the canonical
// tables for the longer ones.
class Huffman {
 public:
  // False when the lengths over-subscribe the code space. Incomplete codes are allowed (DEFLATE
  // permits one distance code of length 1); decoding an unused pattern then fails.
  bool build(const uint8_t* lengths, int n) {
    count_.fill(0);
    fast_.fill(0);
    for (int i = 0; i < n; ++i) ++count_[lengths[i]];
    count_[0] = 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
      left = (left << 1) - count_[len];
      if (left < 0) return false;
    }
    std::array<uint16_t, kMaxBits + 2> offset{};
    for (int len = 1; len <= kMaxBits; ++len) offset[len + 1] = static_cast<uint16_t>(offset[len] + count_[len]);
    std::array<uint16_t, kMaxBits + 1> next{};
    for (int i = 0; i < n; ++i) {
      if (lengths[i]) symbols_[offset[lengths[i]] + next[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    // Fast table: codes are assigned in canonical order; the stream holds them MSB-first, so the
    // table index is the code with its bits reversed.
    uint32_t code = 0;
    int index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
      for (int k = 0; k < count_[len]; ++k, ++index, ++code) {
        if (len > kFastBits) continue;
        const uint32_t rev = reverse_bits(code, len);
        const auto entry = static_cast<uint16_t>((len << 9) | symbols_[index]);
        for (uint32_t fill = rev; fill < (1u << kFastBits); fill += (1u << len)) fast_[fill] = entry;
      }
      code <<= 1;
    }
    return true;
  }

  // The next symbol, or -1 for a pattern no code uses.
  int decode(Bits& bits) const {
    const uint16_t e = fast_[bits.peek(kFastBits)];
    if (e) {
      bits.drop(e >> 9);
      return e & 0x1FF;
    }
    // Longer than kFastBits: walk the canonical code bit by bit.
    const uint32_t window = bits.peek(kMaxBits);
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
      code |= static_cast<int>((window >> (len - 1)) & 1);
      const int count = count_[len];
      if (code - first < count) {
        bits.drop(len);
        return symbols_[index + (code - first)];
      }
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    return -1;
  }

 private:
  std::array<uint16_t, kMaxBits + 1> count_{};
  std::array<uint16_t, 288> symbols_{};
  std::array<uint16_t, 1u << kFastBits> fast_{};
};

const Huffman& fixed_literals() {
  static const Huffman h = [] {
    uint8_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    Huffman t;
    t.build(lengths, 288);
    return t;
  }();
  return h;
}

const Huffman& fixed_distances() {
  static const Huffman h = [] {
    uint8_t lengths[30];
    for (auto& l : lengths) l = 5;
    Huffman t;
    t.build(lengths, 30);
    return t;
  }();
  return h;
}

InflateStatus read_dynamic_tables(Bits& bits, Huffman& literals, Huffman& distances) {
  static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  const int nlit = static_cast<int>(bits.take(5)) + 257;
  const int ndist = static_cast<int>(bits.take(5)) + 1;
  const int ncode = static_cast<int>(bits.take(4)) + 4;
  if (nlit > 286 || ndist > 30) return InflateStatus::Corrupt;

  uint8_t code_lengths[19] = {};
  for (int i = 0; i < ncode; ++i) code_lengths[kOrder[i]] = static_cast<uint8_t>(bits.take(3));
  Huffman lencode;
  if (!lencode.build(code_lengths, 19)) return InflateStatus::Corrupt;

  uint8_t lengths[286 + 30] = {};
  int i = 0;
  while (i < nlit + ndist) {
    const int sym = lencode.decode(bits);
    if (sym < 0) return InflateStatus::Corrupt;
    if (sym < 16) {
      lengths[i++] = static_cast<uint8_t>(sym);
      continue;
    }
    int repeat = 0;
    uint8_t value = 0;
    if (sym == 16) {
      if (i == 0) return InflateStatus::Corrupt;
      value = lengths[i - 1];
      repeat = 3 + static_cast<int>(bits.take(2));
    } else if (sym == 17) {
      repeat = 3 + static_cast<int>(bits.take(3));
    } else {
      repeat = 11 + static_cast<int>(bits.take(7));
    }
    if (i + repeat > nlit + ndist) return InflateStatus::Corrupt;
    while (repeat--) lengths[i++] = value;
  }
  if (lengths[256] == 0) return InflateStatus::Corrupt;  // no end-of-block code
  if (!literals.build(lengths, nlit) || !distances.build(lengths + nlit, ndist)) return InflateStatus::Corrupt;
  return InflateStatus::Ok;
}

InflateStatus inflate_block(Bits& bits, const Huffman& literals, const Huffman& distances, std::span<uint8_t> out,
                            size_t& pos) {
  for (;;) {
    const int sym = literals.decode(bits);
    if (sym < 0) return InflateStatus::Corrupt;
    if (sym < 256) {
      if (pos >= out.size()) return InflateStatus::OutputFull;
      out[pos++] = static_cast<uint8_t>(sym);
      continue;
    }
    if (sym == 256) return InflateStatus::Ok;
    const int li = sym - 257;
    if (li >= 29) return InflateStatus::Corrupt;
    const size_t length = kLengthBase[li] + bits.take(kLengthExtra[li]);
    const int ds = distances.decode(bits);
    if (ds < 0 || ds >= 30) return InflateStatus::Corrupt;
    const size_t distance = kDistBase[ds] + bits.take(kDistExtra[ds]);
    if (distance > pos) return InflateStatus::Corrupt;
    if (length > out.size() - pos) return InflateStatus::OutputFull;
    uint8_t* dst = out.data() + pos;
    const uint8_t* src = dst - distance;
    if (distance >= length) {
      std::memcpy(dst, src, length);
    } else {
      for (size_t k = 0; k < length; ++k) dst[k] = src[k];  // overlapping: repeats the pattern
    }
    pos += length;
  }
}

}  // namespace

const char* inflate_status_name(InflateStatus s) {
  switch (s) {
    case InflateStatus::Ok: return "ok";
    case InflateStatus::Corrupt: return "corrupt";
    case InflateStatus::Truncated: return "truncated";
    case InflateStatus::OutputFull: return "output full";
  }
  return "?";
}

InflateResult inflate_raw(std::span<const uint8_t> in, std::span<uint8_t> out) {
  InflateResult r;
  Bits bits(in);
  size_t pos = 0;
  Huffman literals, distances;
  for (;;) {
    const uint32_t final_block = bits.take(1);
    const uint32_t type = bits.take(2);
    InflateStatus s = InflateStatus::Ok;
    if (type == 0) {
      bits.align_to_byte();
      const uint32_t len = bits.take(16);
      const uint32_t nlen = bits.take(16);
      if ((len ^ 0xFFFF) != nlen) {
        s = InflateStatus::Corrupt;
      } else if (len > out.size() - pos) {
        s = InflateStatus::OutputFull;
      } else {
        for (uint32_t k = 0; k < len; ++k) out[pos++] = static_cast<uint8_t>(bits.take(8));
      }
    } else if (type == 1) {
      s = inflate_block(bits, fixed_literals(), fixed_distances(), out, pos);
    } else if (type == 2) {
      s = read_dynamic_tables(bits, literals, distances);
      if (s == InflateStatus::Ok) s = inflate_block(bits, literals, distances, out, pos);
    } else {
      s = InflateStatus::Corrupt;
    }
    if (bits.overrun()) s = InflateStatus::Truncated;
    if (s != InflateStatus::Ok) {
      r.status = s;
      r.written = pos;
      return r;
    }
    if (final_block) break;
  }
  r.status = InflateStatus::Ok;
  r.written = pos;
  r.consumed = std::min(bits.consumed(), in.size());
  return r;
}

uint32_t adler32(std::span<const uint8_t> data, uint32_t start) {
  uint32_t a = start & 0xFFFF, b = start >> 16;
  size_t i = 0;
  while (i < data.size()) {
    const size_t chunk = std::min<size_t>(data.size() - i, 5552);  // largest run without overflow
    for (size_t k = 0; k < chunk; ++k) {
      a += data[i + k];
      b += a;
    }
    a %= 65521;
    b %= 65521;
    i += chunk;
  }
  return (b << 16) | a;
}

InflateResult inflate_zlib(std::span<const uint8_t> in, std::span<uint8_t> out) {
  InflateResult r;
  if (in.size() < 6) {
    r.status = InflateStatus::Truncated;
    return r;
  }
  const uint8_t cmf = in[0], flg = in[1];
  if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) {
    r.status = InflateStatus::Corrupt;  // not deflate, bad check bits, or a preset dictionary
    return r;
  }
  r = inflate_raw(in.subspan(2), out);
  if (r.status != InflateStatus::Ok) return r;
  const size_t tail = 2 + r.consumed;
  if (tail + 4 > in.size()) {
    r.status = InflateStatus::Truncated;
    return r;
  }
  const uint32_t expected = (uint32_t{in[tail]} << 24) | (uint32_t{in[tail + 1]} << 16) | (uint32_t{in[tail + 2]} << 8) |
                            in[tail + 3];
  if (adler32(out.first(r.written)) != expected) r.status = InflateStatus::Corrupt;
  r.consumed = tail + 4;
  return r;
}

}  // namespace am::formats
