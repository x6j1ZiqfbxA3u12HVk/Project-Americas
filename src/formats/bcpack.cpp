#include "formats/bcpack.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace am::formats::bcpack {

namespace {

constexpr int kModels = 128;
constexpr int kBuckets = 9;
constexpr int kHistory = 256;
constexpr int kDictionary = 255;
constexpr uint32_t kLow = 1u << 24;  // rANS: decode while the state is above, read while below
constexpr int kRoundBytes = 8;      // input bytes the lanes take between them each round

// Endpoints and indices of BC1, BC3, BC4 and BC5: small unsigned values, summed per element
// modulo the element's width.
struct Plain {
  uint8_t ep[8] = {};
  uint8_t ix[32] = {};
};

struct Bc7Ep {
  uint16_t ctrl = 0;
  uint16_t rows[3][8] = {};  // per subset: R0 R1 G0 G1 B0 B1 A0 A1, left-aligned to 16 bits
};
struct Bc7Ix {
  uint8_t bits[2] = {};
  int16_t ix[2][16] = {};
};
struct Bc7 {
  Bc7Ep ep;
  Bc7Ix ix;
};

// BC6H: four endpoints (R, G, B each, left-aligned to 16 bits with the top bit flipped), the
// mode (1-14) and 16 signed indices.
struct Bc6 {
  uint16_t e[4][3] = {};
  uint8_t mode = 0;
  int16_t ix[16] = {};
};

}  // namespace

struct TableData {
  uint32_t freq[kModels][kBuckets] = {};
  uint32_t start[kModels][kBuckets + 1] = {};  // each bucket's first slot; 65536 past the last
  uint8_t bucket[kModels][256] = {};           // the bucket of each 256-slot window's first slot
  uint64_t recip[kModels][kBuckets] = {};      // ceil(2^32 / freq): exact division below 2^16
  uint8_t rank1[256] = {}, rank3a[64] = {}, rank3c[256] = {}, rank4[64] = {}, rank5[64] = {};
  uint16_t ctrl7[512] = {};
  std::vector<Plain> bc1, bc3, bc4, bc5;
  std::vector<Bc6> bc6;
  std::vector<Bc7> bc7;
};

namespace {

// The table file: 128 x 9 u16 model frequencies; the rank tables (BC1 256 B, BC3 alpha 64 B and
// colour 256 B, BC4 64 B, BC5 64 B); 512 u16 BC7 control words; then 255 dictionary entries a
// format in format order: BC1 6 + 16 B, BC3 8 + 32 B, BC4 2 + 16 B, BC5 4 + 32 B, BC6H (u8, 12
// u16, u8, 16 i16), BC7 (u16 control, 24 u16, 2 u8, 32 i16). Little-endian, no padding.
constexpr size_t kTableFileSize = kModels * kBuckets * 2 + 256 + 64 + 256 + 64 + 64 + 512 * 2 +
                                  kDictionary * (6 + 16 + 8 + 32 + 2 + 16 + 4 + 32 + 25 + 33 + 50 + 66);

struct Reader {
  const uint8_t* p;
  uint8_t u8() { return *p++; }
  uint16_t u16() {
    const uint16_t v = static_cast<uint16_t>(p[0] | p[1] << 8);
    p += 2;
    return v;
  }
  void bytes(uint8_t* out, size_t n) {
    std::memcpy(out, p, n);
    p += n;
  }
};

// ---- the lanes ----

struct Lane {
  uint8_t width = 0;  // symbol bits - 1 at the full alphabet
  uint8_t phase = 0;  // 0 header, 1 length (low byte), 2 length's high byte, 3 payload
  bool long_length = false, reset = false, need = false, stalled = false;
  uint8_t model = 0;
  int latch = -1;  // the byte taken, or -1
  int out = -1;    // the symbol decoded, or -1
  uint32_t x = 0, left = 0;

  // The bucket holding `slot`: the last whose start is at or below it.
  static int bucket_of(const TableData& t, int model, uint32_t slot) {
    const uint32_t* start = t.start[model];
    int b = t.bucket[model][slot >> 8];
    while (start[b + 1] <= slot) ++b;
    return b;
  }

  // One step: takes the latched byte (header, length or payload) and decodes at most one symbol.
  bool step(const TableData& t) {
    if (phase != 3) {
      if (latch < 0) return true;
      const uint8_t b = static_cast<uint8_t>(latch);
      latch = -1;
      need = true;
      if (phase == 0) {
        const int shrink = b >> 6;  // the top two bits shrink the alphabet
        if (shrink > width) return false;
        model = static_cast<uint8_t>((width - shrink) * 16 + (b & 15));
        reset = (b & 0x10) != 0;
        long_length = (b & 0x20) != 0;
        left = 0;
        phase = 1;
      } else if (phase == 1 && long_length) {
        left = b;
        phase = 2;
      } else if (phase == 1) {
        left = uint32_t{b} + 1;
        phase = 3;
      } else {
        left = ((uint32_t{b} << 8) | left) + 257;
        phase = 3;
      }
      return true;
    }
    uint32_t state = x;
    if (state < kLow && latch >= 0) {
      --left;
      state = (state << 8) | static_cast<uint32_t>(latch);
      latch = -1;
    }
    need = left != 0 && latch < 0;
    if (state > kLow) {
      const uint32_t slot = state & 0xFFFF;
      const int b = bucket_of(t, model, slot);
      uint32_t sym, f, rem;
      if (b == 0) {  // symbol 0 owns slots 1 .. f[0] - 1 (slot 0 is never used)
        sym = 0;
        f = t.freq[model][0] - 1;
        rem = slot - 1;
      } else {  // bucket b holds 2^(b-1) symbols of frequency f[b] from start[b] on
        const uint32_t offset = slot - t.start[model][b];
        const uint32_t q = static_cast<uint32_t>((uint64_t{offset} * t.recip[model][b]) >> 32);
        sym = (1u << (b - 1)) + q;
        f = t.freq[model][b];
        rem = offset - q * f;
      }
      x = (state >> 16) * f + rem;
      out = static_cast<int>(sym & 0xFF);
    } else if (left != 0) {
      x = state;  // at exactly 2^24 a lane neither reads nor decodes again
    } else {
      // The fragment ends; a byte read in this step does not reach the carried state.
      phase = 0;
      need = latch < 0;
      if (reset) x = 0;
    }
    return true;
  }
};

// Decodes `per` symbols a block on every lane into `records` (one record of
// lanes * per symbols a block). Each round the lanes step in order and take at most 8 input
// bytes between them.
bool run_lanes(const TableData& t, std::span<const uint8_t> widths, int per, size_t blocks, std::span<const uint8_t> src,
               std::vector<uint8_t>& records) {
  std::array<Lane, 16> lanes{};
  const size_t n = widths.size();
  for (size_t j = 0; j < n; ++j) {
    lanes[j].width = widths[j];
    lanes[j].need = true;
  }
  const size_t stride = n * per;
  records.assign(blocks * stride, 0);
  const uint32_t full = (1u << n) - 1;
  size_t pos = 0, at = 0, next = 0;
  uint32_t done = 0;
  int idle = 0;
  while (pos < blocks * per) {
    int taken = 0;
    bool moved = false;
    for (size_t j = 0; j < n; ++j) {
      Lane& lane = lanes[j];
      const uint32_t bit = 1u << j;
      const int before = taken;
      if (!lane.stalled && !lane.step(t)) return false;
      const bool waiting = lane.out >= 0 && (done & bit);
      if (lane.need && !waiting && taken < kRoundBytes && next < src.size()) {
        lane.latch = src[next++];
        ++taken;
      }
      lane.stalled = (lane.need && before >= kRoundBytes) || waiting;
      if (!lane.stalled && lane.out >= 0) {
        records[at + j * per] = static_cast<uint8_t>(lane.out);
        lane.out = -1;
        done |= bit;
        moved = true;
      }
    }
    if (done == full) {
      ++pos;
      at += pos % per == 0 ? stride - per + 1 : 1;
      done = 0;
      moved = true;
    }
    idle = taken > 0 || moved ? 0 : idle + 1;  // nothing read and nothing delivered: stuck
    if (idle > 3) return false;
  }
  return true;
}

// ---- shared helpers ----

// Two 4-bit symbols, the first on the even bits.
uint8_t interleave(uint8_t a, uint8_t b) {
  uint8_t v = 0;
  for (int k = 0; k < 4; ++k) v |= static_cast<uint8_t>(((a >> k) & 1) << (2 * k) | ((b >> k) & 1) << (2 * k + 1));
  return v;
}

// The record's endpoint and index references.
void references(const uint8_t* s, bool paired, int& ep_ref, int& ix_ref) {
  const uint8_t d = paired ? interleave(s[0], s[1]) : s[0];
  const uint8_t ix = paired ? interleave(s[2], s[3]) : s[1];
  const uint8_t delta = static_cast<uint8_t>((d >> 1) ^ static_cast<uint8_t>(-(d & 1)));
  ep_ref = static_cast<uint8_t>(ix + delta);
  ix_ref = ix;
}

// Block `i`'s reference `r`: 0 none, else `r` blocks back, before the first block into the
// dictionary.
template <typename T>
const T& pick(size_t i, int r, const std::vector<T>& history, const std::vector<T>& dictionary, const T& zero) {
  if (r == 0) return zero;
  if (static_cast<size_t>(r) <= i) return history[(i - r) % kHistory];
  return dictionary[r - i - 1];
}

// A zigzag residual (0, -1, 1, -2, ...) modulo 2^bits.
uint8_t zz(uint8_t v, int bits) {
  const uint8_t mask = static_cast<uint8_t>((1u << bits) - 1);
  return static_cast<uint8_t>(((v >> 1) ^ ((v & 1) ? mask : 0)) & mask);
}

// 2-bit index residuals: one symbol per 2x2 quad.
void quads(const uint8_t* rank, const uint8_t* s, int count, uint8_t* ix) {
  for (int q = 0; q < count; ++q) {
    const uint8_t v = rank[s[q]];
    const int base = (q >> 1) * 8 + (q & 1) * 2;
    const int place[4] = {0, 1, 4, 5};
    for (int k = 0; k < 4; ++k) ix[base + place[k]] = (v >> (2 * k)) & 3;
  }
}

// 3-bit index residuals: one symbol per two pixels in raster order.
void pairs(const uint8_t* rank, const uint8_t* s, int count, uint8_t* ix) {
  for (int k = 0; k < count; ++k) {
    const uint8_t v = rank[s[k] & 63];
    ix[2 * k] = v & 7;
    ix[2 * k + 1] = (v >> 4) & 7;
  }
}

// ---- BC1, BC3, BC4, BC5 ----

struct PlainFormat {
  uint8_t ep_bits[8];
  uint8_t ix_bits[32];
  bool paired;
};

constexpr PlainFormat make_plain(std::array<uint8_t, 8> ep, int ix3, int ix2, bool paired) {
  PlainFormat f{};
  for (int k = 0; k < 8; ++k) f.ep_bits[k] = ep[k];
  for (int k = 0; k < ix3 + ix2; ++k) f.ix_bits[k] = k < ix3 ? 3 : 2;
  f.paired = paired;
  return f;
}
constexpr PlainFormat kBc1 = make_plain({5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false);
constexpr PlainFormat kBc3 = make_plain({5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true);
constexpr PlainFormat kBc4 = make_plain({8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false);
constexpr PlainFormat kBc5 = make_plain({8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true);

Plain bc1_residual(const TableData& t, const uint8_t* s) {
  Plain v;
  const int bits[6] = {5, 6, 5, 5, 6, 5};
  for (int k = 0; k < 6; ++k) v.ep[k] = zz(s[2 + k], bits[k]);
  quads(t.rank1, s + 8, 4, v.ix);
  return v;
}
Plain bc3_residual(const TableData& t, const uint8_t* s) {
  Plain v;  // the colour lanes carry each channel's two endpoints together
  const uint8_t ep[8] = {zz(s[14], 5), zz(s[16], 6), zz(s[18], 5), zz(s[15], 5),
                         zz(s[17], 6), zz(s[19], 5), zz(s[4], 8),  zz(s[5], 8)};
  std::memcpy(v.ep, ep, 8);
  pairs(t.rank3a, s + 6, 8, v.ix);
  quads(t.rank3c, s + 20, 4, v.ix + 16);
  return v;
}
Plain bc4_residual(const TableData& t, const uint8_t* s) {
  Plain v;
  v.ep[0] = zz(s[2], 8);
  v.ep[1] = zz(s[3], 8);
  pairs(t.rank4, s + 4, 8, v.ix);
  return v;
}
Plain bc5_residual(const TableData& t, const uint8_t* s) {
  Plain v;
  for (int k = 0; k < 4; ++k) v.ep[k] = zz(s[4 + k], 8);
  pairs(t.rank5, s + 8, 16, v.ix);
  return v;
}

// Indices along the endpoint line -> BC1 and BC4 codes.
constexpr uint32_t kBc1Code[4] = {0, 2, 3, 1};
constexpr uint64_t kBc4Code[8] = {0, 2, 3, 4, 5, 6, 7, 1};

void put_le(uint8_t* out, uint64_t v, int bytes) {
  for (int k = 0; k < bytes; ++k) out[k] = static_cast<uint8_t>(v >> (8 * k));
}

void bc1_color(const uint8_t* ep, const uint8_t* ix, uint8_t* out) {
  const uint32_t c0 = uint32_t{ep[0]} << 11 | uint32_t{ep[1]} << 5 | ep[2];
  const uint32_t c1 = uint32_t{ep[3]} << 11 | uint32_t{ep[4]} << 5 | ep[5];
  uint32_t bits = 0;
  for (int p = 0; p < 16; ++p) bits |= kBc1Code[ix[p] & 3] << (2 * p);
  put_le(out, c0, 2);
  put_le(out + 2, c1, 2);
  put_le(out + 4, bits, 4);
}

// A BC4 half: endpoints are kept 128 below their stored value.
void bc4_alpha(uint8_t e0, uint8_t e1, const uint8_t* ix, uint8_t* out) {
  uint64_t v = uint64_t{static_cast<uint8_t>(e0 ^ 0x80)} | uint64_t{static_cast<uint8_t>(e1 ^ 0x80)} << 8;
  for (int p = 0; p < 16; ++p) v |= kBc4Code[ix[p] & 7] << (16 + 3 * p);
  put_le(out, v, 8);
}

template <typename Residual, typename Pack>
void plain(const std::vector<uint8_t>& records, size_t stride, std::span<uint8_t> dst, size_t block,
           const std::vector<Plain>& dictionary, const PlainFormat& w, Residual residual, Pack pack) {
  std::vector<Plain> history(kHistory);
  const Plain zero;
  const size_t blocks = dst.size() / block;
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t* s = records.data() + i * stride;
    int ep_ref = 0, ix_ref = 0;
    references(s, w.paired, ep_ref, ix_ref);
    const Plain& er = pick(i, ep_ref, history, dictionary, zero);
    const Plain& xr = pick(i, ix_ref, history, dictionary, zero);
    Plain v = residual(s);
    for (int k = 0; k < 8; ++k) v.ep[k] = static_cast<uint8_t>((v.ep[k] + er.ep[k]) & ((1u << w.ep_bits[k]) - 1));
    for (int k = 0; k < 32; ++k) v.ix[k] = static_cast<uint8_t>((v.ix[k] + xr.ix[k]) & ((1u << w.ix_bits[k]) - 1));
    history[i % kHistory] = v;
    pack(v, dst.data() + i * block);
  }
}

// ---- BC7 ----

// The public BC7 anchor indices: the second subset of two; the second and third of three.
constexpr uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
                                  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
                                  2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15};
constexpr uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8, 3, 15, 15, 8,  8, 6,  6,  6, 5,  3,  3, 3, 3,  8,  15, 3,  3,
                                   6, 10, 5,  8,  8, 6, 8,  5,  15, 15, 8, 15, 3, 5,  6, 10, 8, 15, 15, 3,  15, 5,
                                   15, 15, 15, 15, 3, 15, 5, 5,  5,  8, 5,  10, 5, 10, 8, 13, 15, 12, 3,  3};
constexpr uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
                                   15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
                                   3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
// Endpoint bits of R G B A per mode, before p-bits and rotation; index bits of the two sets.
constexpr uint8_t kBc7Bits[8][4] = {{4, 4, 4, 0}, {6, 6, 6, 0}, {5, 5, 5, 0}, {7, 7, 7, 0},
                                    {5, 5, 5, 6}, {7, 7, 7, 8}, {7, 7, 7, 7}, {5, 5, 5, 5}};
constexpr uint8_t kBc7Index[8][2] = {{3, 0}, {3, 0}, {2, 0}, {2, 0}, {3, 2}, {2, 2}, {4, 0}, {2, 0}};

bool has_pbits(int mode) { return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7; }

// Channel precisions: the p-bit is red's lowest bit; rotation swaps alpha with the rotated channel.
std::array<uint8_t, 4> bc7_precision(uint16_t ctrl) {
  const int mode = ctrl & 7, rot = (ctrl >> 3) & 3;
  std::array<uint8_t, 4> p = {kBc7Bits[mode][0], kBc7Bits[mode][1], kBc7Bits[mode][2], kBc7Bits[mode][3]};
  if (rot) std::swap(p[3], p[rot - 1]);
  else if (has_pbits(mode)) ++p[0];
  return p;
}

// A byte's even and odd bits as two zigzag-coded 4-bit residuals.
std::array<int16_t, 2> nibbles(uint8_t b, bool odd_first) {
  auto half = [](uint8_t v) {
    uint8_t r = 0;
    for (int k = 0; k < 4; ++k) r |= static_cast<uint8_t>(((v >> (2 * k)) & 1) << k);
    return r;
  };
  auto z = [](uint8_t u) { return static_cast<int16_t>((u >> 1) ^ -static_cast<int16_t>(u & 1)); };
  const int16_t even = z(half(b)), odd = z(half(static_cast<uint8_t>(b >> 1)));
  return odd_first ? std::array<int16_t, 2>{odd, even} : std::array<int16_t, 2>{even, odd};
}

// An index residual plus its reference, both left-aligned to 16 bits at their own widths, back
// to `bits` bits (signed).
int16_t add_index(int16_t cur, uint8_t bits, int16_t ref, uint8_t ref_bits) {
  auto up = [](int16_t v, uint8_t b) -> uint16_t {
    return b == 0 || b > 16 ? 0 : static_cast<uint16_t>(static_cast<uint16_t>(v) << (16 - b));
  };
  const int16_t sum = static_cast<int16_t>(static_cast<uint16_t>(up(cur, bits) + up(ref, ref_bits)));
  return bits == 0 ? 0 : static_cast<int16_t>(sum >> (16 - bits));
}

// A little-endian bit writer for one 128-bit block.
struct Bits {
  uint64_t w[2] = {};
  uint32_t at = 0;
  void put(uint64_t value, uint32_t n) {
    if (n > 0 && at < 128) {
      value &= n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1;
      const uint32_t word = at / 64, shift = at % 64;
      w[word] |= value << shift;
      if (shift && word == 0 && shift + n > 64) w[1] |= value >> (64 - shift);
    }
    at += n;
  }
};

void bc7_pack(const Bc7Ep& ep, const Bc7Ix& ix, std::array<uint8_t, 4> prec, uint8_t* out) {
  const uint16_t ctrl = ep.ctrl;
  const int mode = ctrl & 7, subsets = (ctrl & 0xFF) >> 6, part = (ctrl >> 8) & 63, rot = (ctrl >> 3) & 3;
  Bits w;
  w.put(uint64_t{1} << mode, mode + 1);
  if (mode == 0) w.put(part, 4);
  else if (mode == 1 || mode == 2 || mode == 3 || mode == 7) w.put(part, 6);
  else if (mode == 4) {
    w.put(rot, 2);
    w.put((ctrl >> 5) & 1, 1);
  } else if (mode == 5) {
    w.put(rot, 2);
  }
  const int ends = 2 * subsets;
  auto field = [&](int row, int col, uint8_t p) -> uint64_t { return ep.rows[row][col] >> (16 - p); };
  int order[4] = {0, 1, 2, 3};
  if (rot) std::swap(order[3], order[rot - 1]);
  for (int ch : order) {
    const uint8_t p = prec[ch];
    if (!p) continue;
    const uint32_t drop = ch == 0 && has_pbits(mode) ? 1 : 0;
    for (int e = 0; e < ends; ++e) w.put(field(e / 2, 2 * ch + e % 2, p) >> drop, p - drop);
  }
  if (mode == 1) {
    for (int row = 0; row < subsets; ++row) w.put(field(row, 0, prec[0]) & 1, 1);
  } else if (has_pbits(mode)) {
    for (int e = 0; e < ends; ++e) w.put(field(e / 2, e % 2, prec[0]) & 1, 1);
  }
  int anchors[3] = {0, -1, -1};
  if (subsets == 2) anchors[1] = kAnchor2[part];
  if (subsets == 3) {
    anchors[1] = kAnchor3a[part];
    anchors[2] = kAnchor3b[part];
  }
  auto indices = [&](int set) {
    const uint8_t bits = ix.bits[set];
    for (int p = 0; p < 16; ++p) {
      const bool anchor = p == anchors[0] || p == anchors[1] || p == anchors[2];
      w.put(static_cast<uint16_t>(ix.ix[set][p]), bits - (anchor ? 1 : 0));
    }
  };
  if (mode == 4) {
    indices(1);
    indices(0);
  } else if (mode == 5) {
    indices(0);
    indices(1);
  } else {
    indices(0);
  }
  put_le(out, w.w[0], 8);
  put_le(out + 8, w.w[1], 8);
}

bool bc7(const std::vector<uint8_t>& records, size_t stride, std::span<uint8_t> dst, const TableData& t) {
  std::vector<Bc7> history(kHistory);
  const Bc7 zero;
  const size_t blocks = dst.size() / 16;
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t* s = records.data() + i * stride;
    int ep_ref = 0, ix_ref = 0;
    references(s, true, ep_ref, ix_ref);
    const uint16_t ctrl = t.ctrl7[(s[31] & 15) << 5 | (s[30] & 31)];
    if (!ctrl) return false;  // an unused control code
    const int mode = ctrl & 7, subsets = (ctrl & 0xFF) >> 6;
    const std::array<uint8_t, 4> prec = bc7_precision(ctrl);
    const bool three = subsets == 3;
    auto or0 = [&](uint8_t v) { return three ? uint8_t{0} : v; };
    auto if3 = [&](uint8_t v) { return three ? v : uint8_t{0}; };
    const uint8_t stored[3][8] = {
        {s[4], s[5], s[6], s[7], s[8], s[9], or0(s[10]), or0(s[11])},
        {s[12], s[13], s[14], s[15], s[16], s[17], or0(s[18]), or0(s[19])},
        {s[20], s[21], if3(s[10]), if3(s[11]), if3(s[18]), if3(s[19]), 0, 0},
    };
    const Bc7Ep& r = pick(i, ep_ref, history, t.bc7, zero).ep;
    Bc7 v;
    v.ep.ctrl = ctrl;
    // Rows the reference lacks build on its first row.
    const int common = std::min(subsets, (r.ctrl & 0xFF) >> 6);
    const int columns = prec[3] > 0 ? 8 : 6;
    for (int c = 0; c < columns; ++c) {
      const uint8_t p = prec[c / 2];
      if (!p) continue;
      const uint16_t mask = static_cast<uint16_t>(0xFFFF << (16 - p));
      for (int row = 0; row < subsets && row < 3; ++row) {
        const uint8_t st = stored[row][c];
        const uint16_t residual = static_cast<uint16_t>(((st >> 1) ^ ((st & 1) ? 0xFFFF : 0)) << (16 - p));
        const uint16_t base = r.rows[row < common ? row : 0][c] & mask;
        v.ep.rows[row][c] = static_cast<uint16_t>(base + residual);
      }
    }
    const Bc7Ix& rx = pick(i, ix_ref, history, t.bc7, zero).ix;
    v.ix.bits[0] = kBc7Index[mode][0];
    v.ix.bits[1] = kBc7Index[mode][1];
    const int offsets[2][4] = {{22, 24, 26, 28}, {12, 14, 16, 20}};
    for (int set = 0; set < 2; ++set) {
      if (!v.ix.bits[set]) continue;
      for (int k = 0; k < 4; ++k) {
        for (int b = 0; b < 2; ++b) {
          const std::array<int16_t, 2> pair = nibbles(s[offsets[set][k] + b], k < 3);
          for (int h = 0; h < 2; ++h) {
            const int p = 4 * k + 2 * b + h;
            v.ix.ix[set][p] = add_index(pair[h], v.ix.bits[set], rx.ix[set][p], rx.bits[set]);
          }
        }
      }
    }
    history[i % kHistory] = v;
    bc7_pack(v.ep, v.ix, prec, dst.data() + i * 16);
  }
  return true;
}

// ---- BC6H ----

// Bits of endpoint 0's channels, then of the other endpoints' R, G and B, for modes 1-14.
constexpr uint8_t kBc6Bits[15][4] = {{0, 0, 0, 0},     {10, 5, 5, 5},   {7, 6, 6, 6},    {11, 5, 4, 4},   {11, 4, 5, 4},
                                     {11, 4, 4, 5},    {9, 5, 5, 5},    {8, 6, 5, 5},    {8, 5, 6, 5},    {8, 5, 5, 6},
                                     {6, 6, 6, 6},     {10, 10, 10, 10}, {11, 9, 9, 9},  {12, 8, 8, 8},   {16, 4, 4, 4}};
// The block's mode field (value, bits) for modes 1-14.
constexpr uint8_t kBc6Code[15][2] = {{0, 0},  {0, 2},  {1, 2},  {2, 5},  {6, 5},  {10, 5}, {14, 5}, {18, 5},
                                     {22, 5}, {26, 5}, {30, 5}, {3, 5},  {7, 5},  {11, 5}, {15, 5}};

// One run of a BC6H header after the mode field: `len` bits from bit `first` of channel `ch`
// (0 R, 1 G, 2 B; 3 is the 5-bit partition) of endpoint `ep`.
struct Bc6Run {
  uint8_t ch, ep, first, len;
};
constexpr uint8_t R = 0, G = 1, B = 2;
constexpr Bc6Run kPart = {3, 0, 0, 5};
// The format's header layouts per mode (modes 13 and 14 store the high base bits in ascending
// order, as the console's packer writes them).
constexpr Bc6Run kBc6Mode1[] = {{G, 2, 4, 1},  {B, 2, 4, 1}, {B, 3, 4, 1}, {R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10},
                                {R, 1, 0, 5},  {G, 3, 4, 1}, {G, 2, 0, 4}, {G, 1, 0, 5},  {B, 3, 0, 1},  {G, 3, 0, 4},
                                {B, 1, 0, 5},  {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 5},  {B, 3, 2, 1},  {R, 3, 0, 5},
                                {B, 3, 3, 1},  kPart};
constexpr Bc6Run kBc6Mode2[] = {{G, 2, 5, 1}, {G, 3, 4, 1}, {G, 3, 5, 1}, {R, 0, 0, 7}, {B, 3, 0, 1}, {B, 3, 1, 1},
                                {B, 2, 4, 1}, {G, 0, 0, 7}, {B, 2, 5, 1}, {B, 3, 2, 1}, {G, 2, 4, 1}, {B, 0, 0, 7},
                                {B, 3, 3, 1}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 6},
                                {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, kPart};
constexpr Bc6Run kBc6Mode3[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 5}, {R, 0, 10, 1}, {G, 2, 0, 4},
                                {G, 1, 0, 4},  {G, 0, 10, 1}, {B, 3, 0, 1},  {G, 3, 0, 4}, {B, 1, 0, 4},  {B, 0, 10, 1},
                                {B, 3, 1, 1},  {B, 2, 0, 4},  {R, 2, 0, 5},  {B, 3, 2, 1}, {R, 3, 0, 5},  {B, 3, 3, 1}, kPart};
constexpr Bc6Run kBc6Mode4[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 1}, {G, 3, 4, 1},
                                {G, 2, 0, 4},  {G, 1, 0, 5},  {G, 0, 10, 1}, {G, 3, 0, 4}, {B, 1, 0, 4},  {B, 0, 10, 1},
                                {B, 3, 1, 1},  {B, 2, 0, 4},  {R, 2, 0, 4},  {B, 3, 0, 1}, {B, 3, 2, 1},  {R, 3, 0, 4},
                                {G, 2, 4, 1},  {B, 3, 3, 1},  kPart};
constexpr Bc6Run kBc6Mode5[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 1}, {B, 2, 4, 1},
                                {G, 2, 0, 4},  {G, 1, 0, 4},  {G, 0, 10, 1}, {B, 3, 0, 1}, {G, 3, 0, 4},  {B, 1, 0, 5},
                                {B, 0, 10, 1}, {B, 2, 0, 4},  {R, 2, 0, 4},  {B, 3, 1, 1}, {B, 3, 2, 1},  {R, 3, 0, 4},
                                {B, 3, 4, 1},  {B, 3, 3, 1},  kPart};
constexpr Bc6Run kBc6Mode6[] = {{R, 0, 0, 9}, {B, 2, 4, 1}, {G, 0, 0, 9}, {G, 2, 4, 1}, {B, 0, 0, 9}, {B, 3, 4, 1},
                                {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4}, {G, 1, 0, 5}, {B, 3, 0, 1}, {G, 3, 0, 4},
                                {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 5}, {B, 3, 2, 1}, {R, 3, 0, 5},
                                {B, 3, 3, 1}, kPart};
constexpr Bc6Run kBc6Mode7[] = {{R, 0, 0, 8}, {G, 3, 4, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {B, 3, 2, 1}, {G, 2, 4, 1},
                                {B, 0, 0, 8}, {B, 3, 3, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 5},
                                {B, 3, 0, 1}, {G, 3, 0, 4}, {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 6},
                                {R, 3, 0, 6}, kPart};
constexpr Bc6Run kBc6Mode8[] = {{R, 0, 0, 8}, {B, 3, 0, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {G, 2, 5, 1}, {G, 2, 4, 1},
                                {B, 0, 0, 8}, {G, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4},
                                {G, 1, 0, 6}, {G, 3, 0, 4}, {B, 1, 0, 5}, {B, 3, 1, 1}, {B, 2, 0, 4}, {R, 2, 0, 5},
                                {B, 3, 2, 1}, {R, 3, 0, 5}, {B, 3, 3, 1}, kPart};
constexpr Bc6Run kBc6Mode9[] = {{R, 0, 0, 8}, {B, 3, 1, 1}, {B, 2, 4, 1}, {G, 0, 0, 8}, {B, 2, 5, 1}, {G, 2, 4, 1},
                                {B, 0, 0, 8}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 5}, {G, 3, 4, 1}, {G, 2, 0, 4},
                                {G, 1, 0, 5}, {B, 3, 0, 1}, {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 5},
                                {B, 3, 2, 1}, {R, 3, 0, 5}, {B, 3, 3, 1}, kPart};
constexpr Bc6Run kBc6Mode10[] = {{R, 0, 0, 6}, {G, 3, 4, 1}, {B, 3, 0, 1}, {B, 3, 1, 1}, {B, 2, 4, 1}, {G, 0, 0, 6},
                                 {G, 2, 5, 1}, {B, 2, 5, 1}, {B, 3, 2, 1}, {G, 2, 4, 1}, {B, 0, 0, 6}, {G, 3, 5, 1},
                                 {B, 3, 3, 1}, {B, 3, 5, 1}, {B, 3, 4, 1}, {R, 1, 0, 6}, {G, 2, 0, 4}, {G, 1, 0, 6},
                                 {G, 3, 0, 4}, {B, 1, 0, 6}, {B, 2, 0, 4}, {R, 2, 0, 6}, {R, 3, 0, 6}, kPart};
constexpr Bc6Run kBc6Mode11[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 10}, {G, 1, 0, 10}, {B, 1, 0, 10}};
constexpr Bc6Run kBc6Mode12[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 9}, {R, 0, 10, 1},
                                 {G, 1, 0, 9},  {G, 0, 10, 1}, {B, 1, 0, 9},  {B, 0, 10, 1}};
constexpr Bc6Run kBc6Mode13[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 8}, {R, 0, 10, 2},
                                 {G, 1, 0, 8},  {G, 0, 10, 2}, {B, 1, 0, 8},  {B, 0, 10, 2}};
constexpr Bc6Run kBc6Mode14[] = {{R, 0, 0, 10}, {G, 0, 0, 10}, {B, 0, 0, 10}, {R, 1, 0, 4}, {R, 0, 10, 6},
                                 {G, 1, 0, 4},  {G, 0, 10, 6}, {B, 1, 0, 4},  {B, 0, 10, 6}};
constexpr std::span<const Bc6Run> kBc6Layout[15] = {
    {},         kBc6Mode1,  kBc6Mode2,  kBc6Mode3,  kBc6Mode4,  kBc6Mode5,  kBc6Mode6, kBc6Mode7,
    kBc6Mode8,  kBc6Mode9,  kBc6Mode10, kBc6Mode11, kBc6Mode12, kBc6Mode13, kBc6Mode14};

uint8_t bc6_index_bits(uint8_t mode) { return mode >= 1 && mode <= 10 ? 3 : mode >= 11 && mode <= 14 ? 4 : 0; }

// Two bytes' bits interleaved into 16: the first on the even bits.
uint16_t interleave16(uint8_t a, uint8_t b) {
  uint16_t v = 0;
  for (int k = 0; k < 8; ++k) v |= static_cast<uint16_t>(((a >> k) & 1) << (2 * k) | ((b >> k) & 1) << (2 * k + 1));
  return v;
}

uint16_t zz16(uint16_t v) { return static_cast<uint16_t>((v >> 1) ^ static_cast<uint16_t>(-(v & 1))); }

// A record: references, the mode (byte 30, less one), endpoint 0 and 1 residuals as interleaved
// byte pairs (4-15), endpoints 2 and 3 (two-subset modes) one byte each (16-21), index nibbles
// (22-29), the partition (31). Endpoint 0 sums at the base precision, the others per channel,
// each cut to its precision after the sum; one-subset modes carry endpoints 2 and 3 for the
// next blocks: endpoint 0 at the channel precision, and endpoint 1.
bool bc6(const std::vector<uint8_t>& records, size_t stride, std::span<uint8_t> dst, const TableData& t) {
  std::vector<Bc6> history(kHistory);
  Bc6 zero;
  zero.mode = 11;
  const size_t blocks = dst.size() / 16;
  for (size_t i = 0; i < blocks; ++i) {
    const uint8_t* s = records.data() + i * stride;
    int ep_ref = 0, ix_ref = 0;
    references(s, true, ep_ref, ix_ref);
    const uint8_t mode = static_cast<uint8_t>(s[30] + 1);
    if (mode < 1 || mode > 14) return false;
    const bool two = mode < 11;
    uint32_t shift[4];
    for (int k = 0; k < 4; ++k) shift[k] = 16u - kBc6Bits[mode][k];
    uint16_t res[4][3] = {};
    for (int c = 0; c < 3; ++c) {
      res[0][c] = zz16(interleave16(s[4 + 2 * c], s[5 + 2 * c]));
      res[1][c] = zz16(interleave16(s[10 + 2 * c], s[11 + 2 * c]));
      res[2][c] = two ? zz16(s[16 + 2 * c]) : res[0][c];
      res[3][c] = two ? zz16(s[17 + 2 * c]) : res[1][c];
    }
    const Bc6& r = pick(i, ep_ref, history, t.bc6, zero);
    Bc6 v;
    v.mode = mode;
    auto mask = [](uint32_t sh) { return static_cast<uint16_t>(0xFFFFu << sh); };
    for (int c = 0; c < 3; ++c) {
      v.e[0][c] = static_cast<uint16_t>((r.e[0][c] + (res[0][c] << shift[0])) & mask(shift[0]));
    }
    for (int k = 1; k < (two ? 4 : 2); ++k) {
      for (int c = 0; c < 3; ++c) {
        v.e[k][c] = static_cast<uint16_t>((r.e[k][c] + (res[k][c] << shift[1 + c])) & mask(shift[1 + c]));
      }
    }
    if (!two) {
      for (int c = 0; c < 3; ++c) v.e[2][c] = static_cast<uint16_t>((v.e[0][c] >> shift[0]) << shift[1]);
      for (int c = 0; c < 3; ++c) v.e[3][c] = v.e[1][c];
    }
    const Bc6& rx = pick(i, ix_ref, history, t.bc6, zero);
    const uint8_t bits = bc6_index_bits(mode);
    for (int k = 0; k < 8; ++k) {
      const std::array<int16_t, 2> pair = nibbles(s[22 + k], k < 6);
      for (int h = 0; h < 2; ++h) {
        const int p = 2 * k + h;
        v.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
      }
    }
    history[i % kHistory] = v;
    // The block: the mode field, the header runs (fields from the working values, top bit
    // flipped back), then the indices (the first, and the second subset's anchor, one bit short).
    auto field = [&](int k, int c) -> uint64_t {
      const uint32_t sh = k == 0 ? shift[0] : shift[1 + c];
      return static_cast<uint16_t>(v.e[k][c] ^ 0x8000) >> sh;
    };
    Bits w;
    w.put(kBc6Code[mode][0], kBc6Code[mode][1]);
    const uint8_t part = s[31] & 31;
    for (const Bc6Run& run : kBc6Layout[mode]) {
      if (run.ch == 3) {
        w.put(part, 5);
      } else {
        w.put(field(run.ep, run.ch) >> run.first, run.len);
      }
    }
    const int anchor = two ? kAnchor2[part] : 0;
    for (int p = 0; p < 16; ++p) {
      const bool short_one = p == 0 || p == anchor;
      const uint16_t value = static_cast<uint16_t>(static_cast<uint16_t>(v.ix[p]) + (1u << (bits - 1)));
      w.put(value, bits - (short_one ? 1u : 0u));
    }
    put_le(dst.data() + i * 16, w.w[0], 8);
    put_le(dst.data() + i * 16 + 8, w.w[1], 8);
  }
  return true;
}

}  // namespace

Tables::Tables() : data_(std::make_unique<TableData>()) {}
Tables::~Tables() = default;

size_t table_file_size() { return kTableFileSize; }

std::unique_ptr<Tables> Tables::load(std::span<const uint8_t> bytes, std::string& error) {
  if (bytes.size() != kTableFileSize) {
    error = "the table file has the wrong size (" + std::to_string(bytes.size()) + " bytes, " +
            std::to_string(kTableFileSize) + " expected)";
    return nullptr;
  }
  std::unique_ptr<Tables> tables(new Tables);
  TableData& t = *tables->data_;
  Reader r{bytes.data()};
  for (int m = 0; m < kModels; ++m) {
    for (int b = 0; b < kBuckets; ++b) t.freq[m][b] = r.u16();
    // Model 16 g + shape has g + 2 buckets; bucket b > 0 holds 2^(b-1) symbols.
    const int used = m / 16 + 2;
    uint32_t at = 0;
    for (int b = 0; b < used; ++b) {
      if (!t.freq[m][b]) {
        error = "a model has an empty bucket";
        return nullptr;
      }
      t.start[m][b] = at;
      t.recip[m][b] = ((uint64_t{1} << 32) + t.freq[m][b] - 1) / t.freq[m][b];
      at += t.freq[m][b] << (b > 0 ? b - 1 : 0);
    }
    if (at != 1u << 16) {
      error = "a model does not tile 65536";
      return nullptr;
    }
    for (int b = used; b <= kBuckets; ++b) t.start[m][b] = 1u << 16;
    for (int window = 0; window < 256; ++window) {
      const uint32_t first = static_cast<uint32_t>(window) << 8;
      int count = 0;
      for (int b = 1; b <= kBuckets; ++b) count += t.start[m][b] <= first ? 1 : 0;
      t.bucket[m][window] = static_cast<uint8_t>(count);
    }
  }
  r.bytes(t.rank1, 256);
  r.bytes(t.rank3a, 64);
  r.bytes(t.rank3c, 256);
  r.bytes(t.rank4, 64);
  r.bytes(t.rank5, 64);
  for (uint16_t& c : t.ctrl7) c = r.u16();
  auto plain_dictionary = [&](std::vector<Plain>& out, int ep, int ix) {
    out.resize(kDictionary);
    for (Plain& p : out) {
      r.bytes(p.ep, ep);
      r.bytes(p.ix, ix);
    }
  };
  plain_dictionary(t.bc1, 6, 16);
  plain_dictionary(t.bc3, 8, 32);
  plain_dictionary(t.bc4, 2, 16);
  plain_dictionary(t.bc5, 4, 32);
  t.bc6.resize(kDictionary);
  for (Bc6& e : t.bc6) {
    r.u8();  // the endpoint entry's own mode byte: not used
    for (auto& end : e.e) {
      for (uint16_t& v : end) v = r.u16();
    }
    e.mode = r.u8();
    for (int16_t& v : e.ix) v = static_cast<int16_t>(r.u16());
  }
  t.bc7.resize(kDictionary);
  for (Bc7& e : t.bc7) {
    e.ep.ctrl = r.u16();
    for (auto& row : e.ep.rows) {
      for (uint16_t& v : row) v = r.u16();
    }
    e.ix.bits[0] = r.u8();
    e.ix.bits[1] = r.u8();
    for (auto& set : e.ix.ix) {
      for (int16_t& v : set) v = static_cast<int16_t>(r.u16());
    }
  }
  return tables;
}

const char* status_name(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Unsupported: return "BC format not done";
    case Status::BadSize: return "destination not a whole number of blocks";
    case Status::Corrupt: return "stream ends early or is corrupt";
  }
  return "?";
}

Status decode(int bc, std::span<const uint8_t> src, std::span<uint8_t> dst, const Tables& tables) {
  const TableData& t = tables.data();
  size_t block = 0;
  int per = 0;
  std::span<const uint8_t> widths;
  static constexpr uint8_t kW1[] = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
  static constexpr uint8_t kW3[] = {3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
  static constexpr uint8_t kW4[] = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
  static constexpr uint8_t kW5[] = {3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
  static constexpr uint8_t kW6[] = {3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
  static constexpr uint8_t kW7[] = {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};
  switch (bc) {
    case 1: block = 8, per = 1, widths = kW1; break;
    case 3: block = 16, per = 2, widths = kW3; break;
    case 4: block = 8, per = 1, widths = kW4; break;
    case 5: block = 16, per = 2, widths = kW5; break;
    case 6: block = 16, per = 2, widths = kW6; break;
    case 7: block = 16, per = 2, widths = kW7; break;
    default: return Status::Unsupported;
  }
  if (dst.empty() || dst.size() % block) return Status::BadSize;
  const size_t blocks = dst.size() / block, stride = widths.size() * per;
  std::vector<uint8_t> records;
  if (!run_lanes(t, widths, per, blocks, src, records)) return Status::Corrupt;
  switch (bc) {
    case 1:
      plain(records, stride, dst, block, t.bc1, kBc1, [&](const uint8_t* s) { return bc1_residual(t, s); },
            [](const Plain& v, uint8_t* out) { bc1_color(v.ep, v.ix, out); });
      return Status::Ok;
    case 3:
      plain(records, stride, dst, block, t.bc3, kBc3, [&](const uint8_t* s) { return bc3_residual(t, s); },
            [](const Plain& v, uint8_t* out) {
              bc4_alpha(v.ep[6], v.ep[7], v.ix, out);
              bc1_color(v.ep, v.ix + 16, out + 8);
            });
      return Status::Ok;
    case 4:
      plain(records, stride, dst, block, t.bc4, kBc4, [&](const uint8_t* s) { return bc4_residual(t, s); },
            [](const Plain& v, uint8_t* out) { bc4_alpha(v.ep[0], v.ep[1], v.ix, out); });
      return Status::Ok;
    case 5:
      plain(records, stride, dst, block, t.bc5, kBc5, [&](const uint8_t* s) { return bc5_residual(t, s); },
            [](const Plain& v, uint8_t* out) {
              bc4_alpha(v.ep[0], v.ep[1], v.ix, out);
              bc4_alpha(v.ep[2], v.ep[3], v.ix + 16, out + 8);
            });
      return Status::Ok;
    case 6: return bc6(records, stride, dst, t) ? Status::Ok : Status::Corrupt;
    default: return bc7(records, stride, dst, t) ? Status::Ok : Status::Corrupt;
  }
}

}  // namespace am::formats::bcpack
