#include "shader/translate.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "shader/rdna2.h"

namespace am::shader {
namespace {

namespace rd = rdna2;
using rd::Field;

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08Xu", v);
  return b;
}
std::string num(uint64_t v) { return std::to_string(v); }

// Runs of lane-masked VGPR writes ("if (AM_ACT != 0u) v[N] = ...;" at one indentation) under
// one test: they write no SGPR, so the lane mask is the same for all of them. One branch for
// the run instead of one per instruction is far less for DXC to work through (its value
// propagation and jump threading grow with the number of blocks).
std::string merge_lane_guards(const std::string& body) {
  static constexpr std::string_view kGuard = "if (AM_ACT != 0u) v[";
  // The indentation of a lane-masked VGPR write, or npos.
  const auto guarded = [](std::string_view l) {
    const size_t ind = l.find_first_not_of(' ');
    if (ind == std::string_view::npos || l.substr(ind, kGuard.size()) != kGuard || l.back() != ';') return std::string_view::npos;
    size_t at = ind + kGuard.size();
    const size_t digits = at;
    while (at < l.size() && l[at] >= '0' && l[at] <= '9') ++at;
    if (at == digits || l.substr(at, 4) != "] = ") return std::string_view::npos;
    return ind;
  };
  std::vector<std::string_view> lines;
  for (size_t at = 0; at < body.size();) {
    const size_t nl = std::min(body.find('\n', at), body.size());
    lines.push_back(std::string_view(body).substr(at, nl - at));
    at = nl + 1;
  }
  std::string out;
  out.reserve(body.size());
  for (size_t i = 0; i < lines.size();) {
    const size_t ind = guarded(lines[i]);
    size_t j = i + 1;
    if (ind != std::string_view::npos) {
      while (j < lines.size() && guarded(lines[j]) == ind) ++j;
    }
    if (j - i < 2) {
      out += lines[i];
      out += '\n';
      ++i;
      continue;
    }
    const std::string pad(ind, ' ');
    out += pad + "if (AM_ACT != 0u) {\n";
    for (; i < j; ++i) {
      out += pad + "  ";
      out += lines[i].substr(ind + kGuard.size() - 2);  // from "v["
      out += '\n';
    }
    out += pad + "}\n";
  }
  return out;
}

// Fills a template's placeholders (A, B, C; X for a single source) in one pass over the
// template: what is put in is never scanned again, so sources may contain those letters
// themselves (hex literals such as 0x3F0A0000u do).
std::string fill(const std::string& t, const std::string& a, const std::string& b = {}, const std::string& c = {}) {
  std::string out;
  out.reserve(t.size() + a.size() + b.size() + c.size());
  for (char ch : t) {
    if (ch == 'A' || ch == 'X') out += a;
    else if (ch == 'B') out += b;
    else if (ch == 'C') out += c;
    else out += ch;
  }
  return out;
}

// ---- the emitter's state for one instruction ----

struct Ctx {
  const rd::Instruction* in = nullptr;
  std::string out;
  const TranslateInput* input = nullptr;  // the program's stage and interface
  bool wave64 = true;
  uint32_t pc = 0;    // dword index of this instruction
  uint32_t next = 0;  // and of the next
  std::string failure;
  bool uses_lds = false;
  uint32_t max_vgpr = 0;
  bool dpp = false;  // this instruction's src0 is am_dpp, and am_dpp_on gates its writes
  bool uses_images = false;
  // Pixel shader attributes read by the 16-bit interpolation instructions: declared
  // nointerpolation and interpolated here, from each vertex's bits and SV_Barycentrics.
  std::set<uint32_t> per_vertex_attrs;
  // The pixel shader exports depth (MRTZ): AmOut gains SV_Depth.
  bool depth_export = false;
  bool uses_lod = false;     // image_get_lod: the level-of-detail helpers
  bool uses_movrel = false;  // v_movrel*: am_nv bounds the indexed VGPRs
  // Descriptor lookups still valid in this block, by first SGPR (T#s, S#s), and every one made.
  std::set<uint32_t> live_images, live_samplers, image_vars, sampler_vars;
  // Forgets the lookups whose registers [lo, hi] may have changed.
  void drop_descriptors(uint32_t lo, uint32_t hi) {
    for (auto* set : {&live_images, &live_samplers}) {
      const uint32_t n = set == &live_images ? 8 : 4;
      for (auto it = set->begin(); it != set->end();) {
        if (*it <= hi && *it + n - 1 >= lo) it = set->erase(it);
        else ++it;
      }
    }
  }

  // The lane condition of a vector write (EXEC, and DPP's masks when it has them).
  std::string act() const { return dpp ? "(AM_ACT != 0u && am_dpp_on)" : "(AM_ACT != 0u)"; }

  uint32_t f(Field x) const { return in->field(x); }
  void line(const std::string& s) { out += "      " + s + "\n"; }
  bool fail(const std::string& why) {
    if (failure.empty()) failure = why;
    return false;
  }
  void touch_v(uint32_t v) { max_vgpr = std::max(max_vgpr, v + 1); }

  // A 32-bit source operand by its 9-bit code (8-bit codes for scalar instructions are the same
  // values). `f16` picks the 16-bit encodings of the inline float constants.
  std::string src(uint32_t code, bool f16 = false) {
    if (code < 106) return "s[" + num(code) + "]";
    if (code == 106 || code == 107 || code == 124 || code == 126 || code == 127) return "s[" + num(code) + "]";
    if (code == 125) return "0u";  // null
    if (code >= 128 && code <= 192) return num(code - 128) + "u";
    if (code >= 193 && code <= 208) return hex32(static_cast<uint32_t>(-static_cast<int32_t>(code - 192)));
    static const uint32_t f32[] = {0x3F000000, 0xBF000000, 0x3F800000, 0xBF800000, 0x40000000, 0xC0000000, 0x40800000, 0xC0800000};
    static const uint32_t f16v[] = {0x3800, 0xB800, 0x3C00, 0xBC00, 0x4000, 0xC000, 0x4400, 0xC400};
    if (code >= 240 && code <= 247) return hex32(f16 ? f16v[code - 240] : f32[code - 240]);
    if (code == 248) return hex32(f16 ? 0x3118 : 0x3E22F983);  // 1 / (2 pi)
    if (code == 251) return "(am_vcc_zero() ? 1u : 0u)";
    if (code == 252) return "(am_exec_zero() ? 1u : 0u)";
    if (code == 253) return "(scc ? 1u : 0u)";
    if (code == 255) return hex32(in->literal());
    if (code >= 256) {
      touch_v(code - 256);
      return "v[" + num(code - 256) + "]";
    }
    fail("source operand " + num(code));
    return "0u";
  }
  // A 64-bit source: a register pair, or a constant widened.
  std::string src64(uint32_t code) {
    if (code < 106 || code == 106 || code == 126) return "am_u64(s[" + num(code) + "], s[" + num(code + 1) + "])";
    if (code >= 128 && code <= 192) return "uint64_t(" + num(code - 128) + ")";
    if (code >= 193 && code <= 208) return "uint64_t(int64_t(-" + num(code - 192) + "))";
    if (code == 255) return "uint64_t(" + hex32(in->literal()) + ")";
    if (code >= 256) {
      touch_v(code - 255);
      return "am_u64(v[" + num(code - 256) + "], v[" + num(code - 255) + "])";
    }
    if (code == 125) return "uint64_t(0)";
    fail("64-bit source operand " + num(code));
    return "uint64_t(0)";
  }
  std::string sdst(uint32_t code) {
    if (code < 106 || code == 106 || code == 107 || code == 124 || code == 126 || code == 127) return "s[" + num(code) + "]";
    if (code == 125) return "am_null";
    fail("scalar destination " + num(code));
    return "am_null";
  }
  std::string vdst(uint32_t reg) {
    touch_v(reg);
    return "v[" + num(reg) + "]";
  }
  void set_s64(uint32_t code, const std::string& expr) {
    if (code == 125) return;
    if (code > 127 || (code > 105 && code != 106 && code != 126)) {
      fail("64-bit scalar destination " + num(code));
      return;
    }
    line("{ uint64_t r = " + expr + "; s[" + num(code) + "] = uint(r); s[" + num(code + 1) + "] = uint(r >> 32); }");
  }
  // A masked VGPR write; SDWA's destination select places the result in its byte or word.
  void vwrite(uint32_t reg, const std::string& expr) {
    std::string e = expr;
    if (in->form == rd::Form::Sdwa && in->encoding != rd::Encoding::Vopc) {
      const uint32_t sel = in->field(Field::DST_SEL), unused = in->field(Field::DST_UNUSED);
      if (sel < 6) {
        const uint32_t sh = sel < 4 ? 8 * sel : 16 * (sel - 4), w = sel < 4 ? 8 : 16;
        e = "am_sdwa_dst(" + vdst(reg) + ", " + e + ", " + num(sh) + "u, " + num(w) + "u, " + num(unused) + "u)";
      }
    }
    line("if " + act() + " " + vdst(reg) + " = " + e + ";");
  }
};

// ---- vector operand access by instruction shape ----

bool is_vop3(const rd::Instruction& i) { return i.encoding == rd::Encoding::Vop3; }

// Source k of a vector ALU instruction (VOP1/2/C, or VOP3).
std::string vsrc(Ctx& c, int k, bool f16 = false) {
  const rd::Instruction& i = *c.in;
  if (i.form == rd::Form::Sdwa && k < 2) {
    // The register is a VGPR, or with S0 / S1 a scalar operand; then the byte or word select.
    const uint32_t reg = c.f(k == 0 ? Field::VSRC0 : Field::VSRC1);
    const bool scalar = c.f(k == 0 ? Field::S0 : Field::S1) != 0;
    std::string x;
    if (scalar) x = c.src(reg, f16);
    else {
      c.touch_v(reg);
      x = "v[" + num(reg) + "]";
    }
    const uint32_t sel = c.f(k == 0 ? Field::SRC0_SEL : Field::SRC1_SEL);
    const bool sext = c.f(k == 0 ? Field::SRC0_SEXT : Field::SRC1_SEXT) != 0;
    if (sel >= 6) return x;
    const uint32_t sh = sel < 4 ? 8 * sel : 16 * (sel - 4), w = sel < 4 ? 8 : 16;
    if (sext) return "asuint(asint(" + x + " << " + num(32 - sh - w) + "u) >> " + num(32 - w) + "u)";
    return "((" + x + " >> " + num(sh) + "u) & " + hex32((1u << w) - 1) + ")";
  }
  if ((i.form == rd::Form::Dpp16 || i.form == rd::Form::Dpp8) && k == 0) return "am_dpp";
  if (is_vop3(i) || i.encoding == rd::Encoding::Vop3p) {
    return c.src(c.f(k == 0 ? Field::SRC0 : k == 1 ? Field::SRC1 : Field::SRC2), f16);
  }
  if (k == 0) return c.src(c.f(Field::SRC0), f16);
  if (k == 1) {
    c.touch_v(c.f(Field::VSRC1));
    return "v[" + num(c.f(Field::VSRC1)) + "]";
  }
  c.fail("a third source outside VOP3");
  return "0u";
}
// The same as a float, with VOP3's abs and neg.
std::string fsrc(Ctx& c, int k) {
  std::string x = "asfloat(" + vsrc(c, k) + ")";
  bool abs = false, neg = false;
  if (is_vop3(*c.in)) {
    abs = (c.f(Field::ABS) >> k) & 1;
    neg = (c.f(Field::NEG) >> k) & 1;
  } else if ((c.in->form == rd::Form::Sdwa || c.in->form == rd::Form::Dpp16) && k < 2) {
    abs = c.f(k == 0 ? Field::SRC0_ABS : Field::SRC1_ABS) != 0;
    neg = c.f(k == 0 ? Field::SRC0_NEG : Field::SRC1_NEG) != 0;
  }
  if (abs) x = "abs(" + x + ")";
  if (neg) x = "(-" + x + ")";
  return x;
}
uint32_t vop_dst(Ctx& c) { return c.f(Field::VDST); }
// A float result with VOP3's output modifier and clamp.
std::string fresult(Ctx& c, const std::string& expr) {
  std::string r = expr;
  if (is_vop3(*c.in) || c.in->form == rd::Form::Sdwa) {
    switch (c.f(Field::OMOD)) {
      case 1: r = "(" + r + ") * 2.0f"; break;
      case 2: r = "(" + r + ") * 4.0f"; break;
      case 3: r = "(" + r + ") * 0.5f"; break;
      default: break;
    }
    if (c.f(Field::CLAMP)) r = "saturate(" + r + ")";
  }
  return "asuint(" + r + ")";
}
bool plain_form(Ctx& c) {
  if (c.in->form == rd::Form::Sdwa) return c.fail(std::string(c.in->info->name) + " (sdwa)");
  if (c.in->form != rd::Form::Plain) return c.fail(std::string(c.in->info->name) + " (dpp)");
  if (is_vop3(*c.in) && c.f(Field::OP_SEL)) return c.fail(std::string(c.in->info->name) + " (op_sel)");
  return true;
}

// DPP: src0 comes from another lane of the wave. Fetched before any EXEC test, so every host
// lane takes part in the read; a source lane out of its row (or, without FI, disabled) gives 0
// with BOUND_CTRL and otherwise leaves the lane unwritten.
bool dpp_prelude(Ctx& c) {
  const rd::Instruction& i = *c.in;
  const uint32_t r = c.f(Field::VSRC0);
  c.touch_v(r);
  std::string lane, valid = "true";
  bool fi, zero_when_out;
  std::string masks = "true";
  if (i.form == rd::Form::Dpp8) {
    const uint32_t sels = (i.words[1] >> 8) & 0xFFFFFF;
    lane = "((am_lane & ~7u) | ((" + hex32(sels) + " >> ((am_lane & 7u) * 3u)) & 7u))";
    fi = c.f(Field::SRC0) == 0xEA;
    zero_when_out = true;
  } else {
    const uint32_t ctrl = c.f(Field::DPP_CTRL), n = ctrl & 15;
    if (ctrl < 0x100) {
      lane = "((am_lane & ~3u) | ((" + num(ctrl) + "u >> ((am_lane & 3u) * 2u)) & 3u))";
    } else if (ctrl > 0x100 && ctrl <= 0x10F) {  // row_shl: lane l reads l + n
      lane = "(am_lane + " + num(n) + "u)";
      valid = "((am_lane & 15u) + " + num(n) + "u < 16u)";
    } else if (ctrl > 0x110 && ctrl <= 0x11F) {  // row_shr: l reads l - n
      lane = "(am_lane - " + num(n) + "u)";
      valid = "((am_lane & 15u) >= " + num(n) + "u)";
    } else if (ctrl > 0x120 && ctrl <= 0x12F) {  // row_ror
      lane = "((am_lane & ~15u) | ((am_lane - " + num(n) + "u) & 15u))";
    } else if (ctrl == 0x140) {
      lane = "((am_lane & ~15u) | (15u - (am_lane & 15u)))";
    } else if (ctrl == 0x141) {
      lane = "((am_lane & ~7u) | (7u - (am_lane & 7u)))";
    } else if (ctrl >= 0x150 && ctrl <= 0x15F) {  // row_share
      lane = "((am_lane & ~15u) | " + num(n) + "u)";
    } else if (ctrl >= 0x160 && ctrl <= 0x16F) {  // row_xmask
      lane = "((am_lane & ~15u) | ((am_lane & 15u) ^ " + num(n) + "u))";
    } else {
      return c.fail(std::string(i.info->name) + " (dpp control " + hex32(ctrl) + ")");
    }
    fi = c.f(Field::FI) != 0;
    zero_when_out = c.f(Field::BOUND_CTRL) != 0;
    const uint32_t rm = c.f(Field::ROW_MASK), bm = c.f(Field::BANK_MASK);
    if (rm != 0xF || bm != 0xF) {
      masks = "(((" + num(rm) + "u >> (am_lane >> 4)) & 1u) != 0u && ((" + num(bm) + "u >> ((am_lane >> 2) & 3u)) & 1u) != 0u)";
    }
  }
  c.line("{ uint l = " + lane + "; bool ok = " + valid + ";");
  c.line("  am_dpp = WaveReadLaneAt(v[" + num(r) + "], ok ? l : am_lane);");
  if (!fi) c.line("  ok = ok && am_lane_on(l);");
  if (zero_when_out) c.line("  if (!ok) am_dpp = 0u; am_dpp_on = " + masks + "; }");
  else c.line("  am_dpp_on = ok && " + masks + "; }");
  c.dpp = true;
  return true;
}

bool vector_form(Ctx& c) {
  if (is_vop3(*c.in) && c.f(Field::OP_SEL)) return c.fail(std::string(c.in->info->name) + " (op_sel)");
  if (c.in->form == rd::Form::Dpp16 || c.in->form == rd::Form::Dpp8) return dpp_prelude(c);
  return true;
}

// MUBUF / MTBUF: opens a lane's block with the V# in `rs`, the address in `p` and the bytes
// in bounds from there in `lim`.
bool buffer_address(Ctx& c) {
  if (c.f(Field::LDS) || c.f(Field::TFE)) return c.fail("buffer access with lds/tfe");
  const uint32_t r = c.f(Field::SRSRC) * 4, va = c.f(Field::VADDR);
  const bool idx = c.f(Field::IDXEN), off = c.f(Field::OFFEN);
  c.touch_v(va + (idx && off ? 1 : 0));
  const std::string index = idx ? "v[" + num(va) + "]" : "0u";
  const std::string offset = off ? "v[" + num(va + (idx ? 1 : 0)) + "]" : "0u";
  c.line("if (AM_ACT) { uint4 rs = uint4(s[" + num(r) + "], s[" + num(r + 1) + "], s[" + num(r + 2) + "], s[" + num(r + 3) + "]);");
  c.line("  uint64_t p; uint lim; am_buffer_address(rs, " + index + ", " + offset + " + " + num(c.f(Field::OFFSET)) + "u, " +
         c.src(c.f(Field::SOFFSET)) + ", p, lim);");
  return true;
}

// ---- images (MIMG) ----
//
// The T# and S# are looked up in the host's bindings table (image_bindings.h) where the
// instruction runs, outside any EXEC test: they are wave-uniform, and implicit derivatives
// need every lane. Only the register writes, stores and atomics follow EXEC.

enum class ImgKind : uint8_t { Sample, Gather, Load, Store, Atomic, Info, Lod };
struct ImgOp {
  ImgKind kind;
  bool offset = false, bias = false, compare = false, grads = false, lod = false, implicit = false;
  uint32_t atomic = 0;
};

const rd::Field kNsaFields[] = {Field::VADDRA, Field::VADDRB, Field::VADDRC, Field::VADDRD, Field::VADDRE, Field::VADDRF,
                                Field::VADDRG, Field::VADDRH, Field::VADDRI, Field::VADDRJ, Field::VADDRK, Field::VADDRL};

bool image(Ctx& c, const ImgOp& op) {
  const std::string name = c.in->info->name;
  if (c.f(Field::TFE) || c.f(Field::LWE)) return c.fail(name + " (tfe/lwe)");
  if (c.f(Field::R128)) return c.fail(name + " (128-bit T#)");
  const uint32_t dim = c.f(Field::DIM), dmask = c.f(Field::DMASK), vd = c.f(Field::VDATA);
  const bool a16 = c.f(Field::A16) != 0, d16 = c.f(Field::D16) != 0, nsa = c.f(Field::NSA) != 0;
  const bool sampled = op.kind == ImgKind::Sample || op.kind == ImgKind::Gather || op.kind == ImgKind::Lod;
  if (dim >= 6 && op.kind != ImgKind::Load) return c.fail(name + " (msaa)");
  // Address components, in the hardware's order: offset, bias, z-compare (a dword each), then
  // gradients, coordinates, lod / mip. A16 packs these last two to a register (as halves:
  // f16 for sampling, integers for loads and stores); the bias is then an f16 in its dword's
  // low half, and an odd count of gradients a direction (1D, 3D) pads each direction.
  int next = 0, extra = 0;
  bool bad = false;
  const auto reg_at = [&](int dword) -> std::string {
    uint32_t reg;
    if (!nsa) reg = c.f(Field::VADDR) + dword;
    else if (dword == 0) reg = c.f(Field::VADDR);
    else if (dword - 1 < 12) reg = c.f(kNsaFields[dword - 1]);
    else {
      bad = true;
      return "0u";
    }
    c.touch_v(reg);
    return "v[" + num(reg) + "]";
  };
  const auto whole = [&]() -> std::string { return reg_at(extra++); };
  const auto addr = [&]() -> std::string {
    const int k = next++;
    if (!a16) return reg_at(extra + k);
    const std::string h = "((" + reg_at(extra + k / 2) + " >> " + (k % 2 ? "16u" : "0u") + ") & 0xFFFFu)";
    return sampled ? "asuint(f16tof32(" + h + "))" : h;
  };
  const auto align = [&]() {
    if (a16) next += next % 2;
  };
  static const int kCoords[] = {1, 2, 3, 3, 2, 3, 3, 4}, kGrads[] = {1, 2, 3, 2, 1, 2, 0, 0};
  std::string offset, bias, cmp, g[6], co[4], lod;
  if (op.kind == ImgKind::Info) {
    lod = addr();
  } else {
    if (op.offset) offset = whole();
    if (op.bias) bias = a16 ? "asuint(f16tof32(" + whole() + " & 0xFFFFu))" : whole();
    if (op.compare) cmp = whole();
    if (op.grads) {
      for (int j = 0; j < 2 * kGrads[dim]; ++j) {
        g[j] = addr();
        if (j == kGrads[dim] - 1) align();
      }
      align();
    }
    for (int j = 0; j < kCoords[dim]; ++j) co[j] = addr();
    if (op.lod) lod = addr();
  }
  if (bad) return c.fail(name + " (address)");
  c.uses_images = true;
  const uint32_t t = c.f(Field::SRSRC) * 4, sa = c.f(Field::SSAMP) * 4;
  const auto sq = [&](uint32_t b) { return "uint4(s[" + num(b) + "], s[" + num(b + 1) + "], s[" + num(b + 2) + "], s[" + num(b + 3) + "])"; };
  const std::string suffix = dim == 0 || dim == 4 ? "1d" : dim == 1 || dim == 5 ? "2d" : dim == 2 ? "3d" : dim == 3 ? "cube" : "ms";
  const bool arr = dim == 4 || dim == 5;
  const auto fl = [](const std::string& x) { return "asfloat(" + x + ")"; };
  const auto in = [](const std::string& x) { return "asint(" + x + ")"; };
  // The coordinates for the dimension's helpers: 1d (x, slice), 2d (x, y, slice), 3d (x, y, z),
  // cube (s, t, face), ms (x, y, slice, sample).
  std::string fco, ico;
  switch (dim) {
    case 0: fco = "float2(" + fl(co[0]) + ", 0.0f)"; ico = "int4(" + in(co[0]) + ", 0, 0, 0)"; break;
    case 4: fco = "float2(" + fl(co[0]) + ", " + fl(co[1]) + ")"; ico = "int4(" + in(co[0]) + ", " + in(co[1]) + ", 0, 0)"; break;
    case 1:
      fco = "float3(" + fl(co[0]) + ", " + fl(co[1]) + ", 0.0f)";
      ico = "int4(" + in(co[0]) + ", " + in(co[1]) + ", 0, 0)";
      break;
    case 6: ico = "int4(" + in(co[0]) + ", " + in(co[1]) + ", 0, " + in(co[2]) + ")"; break;
    case 7: ico = "int4(" + in(co[0]) + ", " + in(co[1]) + ", " + in(co[2]) + ", " + in(co[3]) + ")"; break;
    default:  // 3D, cube, 2D array
      fco = "float3(" + fl(co[0]) + ", " + fl(co[1]) + ", " + fl(co[2]) + ")";
      ico = "int4(" + in(co[0]) + ", " + in(co[1]) + ", " + in(co[2]) + ", 0)";
      break;
  }
  (void)arr;
  // A lookup made earlier in the block, of registers unchanged since, is used again.
  if (!c.live_images.count(t)) {
    c.line("am_im_" + num(t) + " = am_image(" + sq(t) + ", " + sq(t + 4) + ");");
    c.live_images.insert(t);
    c.image_vars.insert(t);
  }
  if (sampled && !c.live_samplers.count(sa)) {
    c.line("am_sm_" + num(sa) + " = am_sampler(" + sq(sa) + ");");
    c.live_samplers.insert(sa);
    c.sampler_vars.insert(sa);
  }
  c.line("{ AmImage im = am_im_" + num(t) + ";");
  if (sampled) c.line("  AmSampler sm = am_sm_" + num(sa) + ";");
  // The channels the instruction moves, in order.
  std::vector<int> channels;
  for (int b = 0; b < 4; ++b) {
    if ((dmask >> b) & 1) channels.push_back(b);
  }
  // Writes `n` result channels (uint4 r) to VDATA, two to a register with D16.
  // `float_bits`: the values are float bits (else the image's class decides how D16 packs).
  const auto write_results = [&](const std::vector<int>& from, bool float_bits) {
    if (!d16) {
      c.touch_v(vd + static_cast<uint32_t>(from.size()) - 1);
      std::string w = "  if (AM_ACT) {";
      for (size_t n = 0; n < from.size(); ++n) w += " v[" + num(vd + n) + "] = r[" + num(from[n]) + "];";
      c.line(w + " }");
      return;
    }
    c.touch_v(vd + static_cast<uint32_t>((from.size() + 1) / 2) - 1);
    const std::string h = float_bits ? "am_f16(asfloat(X))" : "((im.flags & 3u) == 0u ? am_f16(asfloat(X)) : (X & 0xFFFFu))";
    std::string w = "  if (AM_ACT) {";
    for (size_t n = 0; n < from.size(); n += 2) {
      std::string e = fill(h, "r[" + num(from[n]) + "]");
      if (n + 1 < from.size()) e = "(" + e + " | (" + fill(h, "r[" + num(from[n + 1]) + "]") + " << 16))";
      w += " v[" + num(vd + n / 2) + "] = " + e + ";";
    }
    c.line(w + " }");
  };

  switch (op.kind) {
    case ImgKind::Sample: {
      if (channels.empty()) channels.push_back(0);
      if (dim == 3) c.line("  float4 cc = am_cube_dir(" + fco + ");");
      else c.line("  " + std::string(dim == 0 || dim == 4 ? "float2" : "float3") + " cc = " + fco + ";");
      const std::string pos = dim == 3 ? "cc" : "cc";
      // The coordinates gradients are taken of: the face direction for cubes.
      const std::string gsel = dim == 3 ? "cc.xyz" : dim == 0 || dim == 4 ? "cc.x" : dim == 2 ? "cc" : "cc.xy";
      const std::string gtype = dim == 3 || dim == 2 ? "float3" : dim == 0 || dim == 4 ? "float" : "float2";
      if (op.compare) {
        if (!op.lod && op.implicit) return c.fail(name);
        if (dim == 2) return c.fail(name + " (3D compare)");
        c.line("  float4 rf = am_cmp_lz_" + suffix + "(im, sm, " + pos + ", " + fl(cmp) + ").xxxx;");
      } else if (op.implicit || op.grads) {
        if (op.implicit) {
          const std::string scale = op.bias ? " * exp2(" + fl(bias) + ")" : "";
          c.line("  " + gtype + " dx = AM_DX(" + gsel + ")" + scale + ", dy = AM_DY(" + gsel + ")" + scale + ";");
        } else {
          const int n = kGrads[dim];
          std::string dx = gtype + "(", dy = gtype + "(";
          for (int j = 0; j < n; ++j) {
            dx += (j ? ", " : "") + fl(g[j]);
            dy += (j ? ", " : "") + fl(g[n + j]);
          }
          dx += ")";
          dy += ")";
          if (dim == 3) {
            // Face-coordinate gradients become direction gradients (the map is affine).
            c.line("  float3 dx = am_cube_dir(" + fco + " + float3(" + fl(g[0]) + ", " + fl(g[1]) + ", 0.0f)).xyz - cc.xyz;");
            c.line("  float3 dy = am_cube_dir(" + fco + " + float3(" + fl(g[2]) + ", " + fl(g[3]) + ", 0.0f)).xyz - cc.xyz;");
          } else {
            c.line("  " + gtype + " dx = " + dx + ", dy = " + dy + ";");
          }
        }
        c.line("  float4 rf = am_grad_" + suffix + "(im, sm, " + pos + ", dx, dy);");
      } else {
        c.line("  float4 rf = am_sample_" + suffix + "(im, sm, " + pos + ", " + (op.lod ? fl(lod) : std::string("0.0f")) + ");");
      }
      c.line("  uint4 r = asuint(rf);");
      write_results(channels, true);
      break;
    }
    case ImgKind::Lod: {
      // The level implicit derivatives select: x clamped to the image's levels, y unclamped.
      if (channels.empty()) channels.push_back(0);
      if (dim == 3) c.line("  float4 cc = am_cube_dir(" + fco + ");");
      else c.line("  " + std::string(dim == 0 || dim == 4 ? "float2" : "float3") + " cc = " + fco + ";");
      const std::string gsel = dim == 3 ? "cc.xyz" : dim == 0 || dim == 4 ? "cc.x" : dim == 2 ? "cc" : "cc.xy";
      const std::string gtype = dim == 3 || dim == 2 ? "float3" : dim == 0 || dim == 4 ? "float" : "float2";
      c.line("  " + gtype + " dx = AM_DX(" + gsel + "), dy = AM_DY(" + gsel + ");");
      c.uses_lod = true;
      c.line("  uint4 r = asuint(float4(am_lod_" + suffix + "(im, sm, dx, dy), 0.0f, 0.0f));");
      write_results(channels, true);
      break;
    }
    case ImgKind::Gather: {
      if (channels.size() != 1) return c.fail(name + " (dmask)");
      if (dim != 1 && dim != 5 && dim != 3) return c.fail(name + " (dimension)");
      std::string off = "int2(0, 0)";
      if (op.offset) {
        if (dim == 3) return c.fail(name + " (cube offset)");
        off = "int2(int(" + offset + " << 26) >> 26, int(" + offset + " << 18) >> 26)";
      }
      if (dim == 3) c.line("  uint4 r = am_gather_cube(im, sm, am_cube_dir(" + fco + "), " + num(channels[0]) + "u);");
      else c.line("  uint4 r = am_gather_2d(im, sm, " + fco + ", " + num(channels[0]) + "u, " + off + ");");
      write_results({0, 1, 2, 3}, false);
      break;
    }
    case ImgKind::Load: {
      if (channels.empty()) channels.push_back(0);
      c.line("  uint4 r = am_load_" + suffix + "(im, " + ico + ", " + (op.lod ? in(lod) : std::string("0")) + ");");
      write_results(channels, false);
      break;
    }
    case ImgKind::Info: {
      if (dim >= 6) return c.fail(name + " (msaa)");
      c.line("  uint4 r = am_info_" + suffix + "(im, " + lod + ");");
      write_results(channels, false);
      break;
    }
    case ImgKind::Store: {
      if (op.lod) return c.fail(name + " (mip)");
      std::string d = "uint4(";
      for (int b = 0, n = 0; b < 4; ++b) {
        std::string x = "0u";
        if ((dmask >> b) & 1) {
          if (!d16) {
            c.touch_v(vd + n);
            x = "v[" + num(vd + n) + "]";
          } else {
            c.touch_v(vd + n / 2);
            const std::string h = "((v[" + num(vd + n / 2) + "] >> " + (n % 2 ? "16u" : "0u") + ") & 0xFFFFu)";
            x = "((im.flags & 3u) == 0u ? asuint(f16tof32(" + h + ")) : " + h + ")";
          }
          ++n;
        }
        d += (b ? ", " : "") + x;
      }
      c.line("  if (AM_ACT) am_store_" + suffix + "(im, " + ico + ", " + d + "));");
      break;
    }
    case ImgKind::Atomic: {
      if (dim != 0 && dim != 1 && dim != 2 && dim != 5) return c.fail(name + " (dimension)");
      c.touch_v(vd + (op.atomic == 1 ? 1 : 0));
      const std::string cmpv = op.atomic == 1 ? "v[" + num(vd + 1) + "]" : "0u";
      c.line("  if (AM_ACT) { uint o = am_atomic_" + suffix + "(im, " + ico + ", " + num(op.atomic) + "u, v[" + num(vd) + "], " +
             cmpv + ");" + (c.f(Field::GLC) ? " v[" + num(vd) + "] = o;" : "") + " }");
      break;
    }
  }
  c.line("}");
  return true;
}

// D16 typed buffer access (the format from the V# or the instruction): `hi` uses the upper half.
bool buffer_d16(Ctx& c, int n, bool store, bool tbuffer, bool hi) {
  if (!buffer_address(c)) return false;
  const uint32_t vd = c.f(Field::VDATA);
  c.touch_v(vd + (n - 1) / 2);
  const std::string fmt = tbuffer ? num(c.f(Field::FORMAT)) + "u" : "((rs.w >> 12) & 0x7Fu)";
  c.line("  uint info = am_fmt_info(" + fmt + "); bool ints = (info & 7u) == 4u || (info & 7u) == 5u;");
  const auto half_of = [&](int k) {
    const std::string v = "v[" + num(vd + k / 2) + "]";
    return "((" + v + " >> " + (k % 2 || hi ? "16u" : "0u") + ") & 0xFFFFu)";
  };
  if (store) {
    std::string val = "uint4(";
    for (int k = 0; k < 4; ++k) {
      const std::string h = k < n ? half_of(k) : std::string("0u");
      val += (k ? ", " : "") + (k < n ? "(ints ? (((info & 7u) == 5u) ? asuint(int(" + h + " << 16) >> 16) : " + h +
                                         ") : asuint(f16tof32(" + h + ")))"
                                       : std::string("0u"));
    }
    c.line("  am_fmt_store(p, lim, info, " + num(n) + "u, " + val + "));");
  } else {
    c.line("  uint4 r = am_fmt_load(p, lim, info, rs.w);");
    c.line("  uint4 h = ints ? (r & 0xFFFFu) : uint4(am_f16(asfloat(r.x)), am_f16(asfloat(r.y)), am_f16(asfloat(r.z)), am_f16(asfloat(r.w)));");
    if (n == 1) {
      const std::string v = "v[" + num(vd) + "]";
      c.line("  " + v + " = " + (hi ? "(" + v + " & 0xFFFFu) | (h.x << 16);" : "(" + v + " & 0xFFFF0000u) | h.x;"));
    } else {
      static const char* const kc = "xyzw";
      for (int k = 0; k < n; k += 2) {
        c.line("  v[" + num(vd + k / 2) + "] = h." + std::string(1, kc[k]) +
               (k + 1 < n ? " | (h." + std::string(1, kc[k + 1]) + " << 16)" : std::string(" | (v[" + num(vd + k / 2) + "] & 0xFFFF0000u)")) + ";");
      }
    }
  }
  c.line("}");
  return true;
}

// ---- 16-bit ALU ----
using Handler = std::function<bool(Ctx&)>;
//
// A 16-bit source is the low half of its register, or with VOP3's op_sel bit the high one. The
// result goes to the low half (op_sel bit 3: the high one) and the other half keeps its bits:
// this GPU (GFX9 and later) preserves them. Inline float constants are the f16 ones.
bool form16(Ctx& c) {
  if (c.in->form == rd::Form::Dpp16 || c.in->form == rd::Form::Dpp8) return dpp_prelude(c);
  return true;
}
std::string src16(Ctx& c, int k) {
  const std::string x = vsrc(c, k, true);
  const bool hi = is_vop3(*c.in) && ((c.f(Field::OP_SEL) >> k) & 1);
  return "((" + x + (hi ? " >> 16u" : "") + ") & 0xFFFFu)";
}
std::string fsrc16(Ctx& c, int k) {
  std::string x = "f16tof32(" + src16(c, k) + ")";
  bool abs = false, neg = false;
  if (is_vop3(*c.in)) {
    abs = (c.f(Field::ABS) >> k) & 1;
    neg = (c.f(Field::NEG) >> k) & 1;
  } else if ((c.in->form == rd::Form::Sdwa || c.in->form == rd::Form::Dpp16) && k < 2) {
    abs = c.f(k == 0 ? Field::SRC0_ABS : Field::SRC1_ABS) != 0;
    neg = c.f(k == 0 ? Field::SRC0_NEG : Field::SRC1_NEG) != 0;
  }
  if (abs) x = "abs(" + x + ")";
  if (neg) x = "(-" + x + ")";
  return x;
}
void write16(Ctx& c, const std::string& value16) {
  const uint32_t d = vop_dst(c);
  const std::string v = c.vdst(d), x = "((" + value16 + ") & 0xFFFFu)";
  if (c.in->form == rd::Form::Sdwa) return c.vwrite(d, x);  // the destination select places it
  const bool hi = is_vop3(*c.in) && ((c.f(Field::OP_SEL) >> 3) & 1);
  c.vwrite(d, hi ? "((" + v + " & 0xFFFFu) | (" + x + " << 16))" : "((" + v + " & 0xFFFF0000u) | " + x + ")");
}
std::string f16_result(Ctx& c, const std::string& f) {
  return "am_f16(" + (is_vop3(*c.in) && c.f(Field::CLAMP) ? "saturate(" + f + ")" : f) + ")";
}
// Integer forms: A, B, C are 16-bit sources.
void int16(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!form16(c)) return false;
    const std::string e = body;
    const std::string a = src16(c, 0), b = e.find('B') != std::string::npos ? src16(c, 1) : "";
    const std::string d = e.find('C') != std::string::npos ? src16(c, 2) : "";
    write16(c, fill(e, a, b, d));
    return true;
  };
}
// Float forms: A, B, C are f16 sources as float.
void float16(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!form16(c)) return false;
    const std::string e = body;
    const std::string a = fsrc16(c, 0), b = e.find('B') != std::string::npos ? fsrc16(c, 1) : "";
    const std::string d = e.find('C') != std::string::npos ? fsrc16(c, 2) : "";
    write16(c, f16_result(c, fill(e, a, b, d)));
    return true;
  };
}
// Packed (VOP3P): both halves at once; per source, op_sel picks the half the low result
// reads and op_sel_hi the one the high result reads; NEG and NEG_HI negate them.
void packed16(std::unordered_map<std::string, Handler>& t, const char* name, const char* body, bool fl, int sources) {
  t[name] = [body, fl, sources](Ctx& c) {
    const uint32_t sel = c.f(Field::OP_SEL), selhi = c.f(Field::OP_SEL_HI) | c.f(Field::OP_SEL_HI_2) << 2;
    const uint32_t neg = c.f(Field::NEG), neghi = c.f(Field::NEG_HI);
    const rd::Field fields[] = {Field::SRC0, Field::SRC1, Field::SRC2};
    std::string lo[3], hi[3];
    for (int k = 0; k < sources; ++k) {
      const uint32_t code = c.f(fields[k]);
      if (code >= 128 && code < 256 && code != 253 && code != 251 && code != 252) {
        return c.fail(std::string(c.in->info->name) + " (constant operand)");
      }
      const std::string x = c.src(code);
      for (int h = 0; h < 2; ++h) {
        const bool upper = ((h ? selhi : sel) >> k) & 1;
        std::string v = "((" + x + (upper ? " >> 16u" : "") + ") & 0xFFFFu)";
        if (fl) {
          v = "f16tof32(" + v + ")";
          if (((h ? neghi : neg) >> k) & 1) v = "(-" + v + ")";
        }
        (h ? hi[k] : lo[k]) = v;
      }
    }
    const auto one = [&](const std::string* v) {
      std::string e = fill(body, v[0], v[1], v[2]);
      if (fl) e = "am_f16(" + (c.f(Field::CLAMP) ? "saturate(" + e + ")" : e) + ")";
      return "((" + e + ") & 0xFFFFu)";
    };
    c.vwrite(vop_dst(c), "(" + one(lo) + " | (" + one(hi) + " << 16))");
    return true;
  };
}

// ---- the instruction table ----

using Handler = std::function<bool(Ctx&)>;

void float_unary(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!vector_form(c)) return false;
    c.vwrite(vop_dst(c), fresult(c, fill(body, fsrc(c, 0))));
    return true;
  };
}
void float_binary(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!vector_form(c)) return false;
    const std::string a = fsrc(c, 0), b = fsrc(c, 1);
    c.vwrite(vop_dst(c), fresult(c, fill(body, a, b)));
    return true;
  };
}
void float_ternary(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!vector_form(c) || !is_vop3(*c.in)) return c.fail(std::string(c.in->info->name));
    const std::string a = fsrc(c, 0), b = fsrc(c, 1), d = fsrc(c, 2);
    c.vwrite(vop_dst(c), fresult(c, fill(body, a, b, d)));
    return true;
  };
}
// Integer forms: A, B, C are uint sources; `vop3_only` for three-source ones.
void int_op(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!vector_form(c)) return false;
    const std::string e = body;
    const bool three = e.find('C') != std::string::npos;
    if (three && !is_vop3(*c.in)) return c.fail(std::string(c.in->info->name));
    const std::string a = vsrc(c, 0);
    const std::string b = e.find('B') != std::string::npos ? vsrc(c, 1) : "";
    const std::string d = three ? vsrc(c, 2) : "";
    c.vwrite(vop_dst(c), fill(e, a, b, d));
    return true;
  };
}

// Compares: VOPC writes VCC (v_cmpx: EXEC), VOP3 the SGPR(s) in its VDST field.
void compare(std::unordered_map<std::string, Handler>& t, const std::string& name, const std::string& cond, bool fl,
             bool is64 = false, bool f16 = false) {
  for (const bool x : {false, true}) {
    const std::string full = (x ? "v_cmpx_" : "v_cmp_") + name;
    t[full] = [cond, fl, x, is64, f16](Ctx& c) {
      if (!vector_form(c)) return false;
      std::string a, b;
      if (f16) {  // the low halves
        a = fsrc16(c, 0);
        b = fsrc16(c, 1);
      } else if (is64) {
        a = c.src64(c.f(Field::SRC0));
        b = is_vop3(*c.in) ? c.src64(c.f(Field::SRC1)) : "am_u64(v[" + num(c.f(Field::VSRC1)) + "], v[" + num(c.f(Field::VSRC1) + 1) + "])";
      } else if (fl) {
        a = fsrc(c, 0);
        b = fsrc(c, 1);
      } else {
        a = vsrc(c, 0);
        b = vsrc(c, 1);
      }
      c.line("{ uint4 m = WaveActiveBallot((" + fill(cond, a, b) + ") && " + c.act() + ");");
      uint32_t dst = is_vop3(*c.in) ? c.f(Field::VDST) : 106;
      if (c.in->form == rd::Form::Sdwa && c.f(Field::SD)) dst = c.f(Field::SDST);
      if (x) {
        c.line("  s[126] = m.x;" + std::string(c.wave64 ? " s[127] = m.y;" : ""));
      } else if (dst != 125) {
        if (dst > 105 && dst != 106 && dst != 126) return c.fail("compare destination " + num(dst));
        c.line("  s[" + num(dst) + "] = m.x;" + (c.wave64 ? " s[" + num(dst + 1) + "] = m.y;" : std::string()));
      }
      c.line("}");
      return true;
    };
  }
}

const std::unordered_map<std::string, Handler>& table() {
  static const std::unordered_map<std::string, Handler> handlers = [] {
    std::unordered_map<std::string, Handler> t;

    // -- vector floats --
    float_binary(t, "v_add_f32", "A + B");
    float_binary(t, "v_sub_f32", "A - B");
    float_binary(t, "v_subrev_f32", "B - A");
    float_binary(t, "v_mul_f32", "A * B");
    // DX9 rules: 0 times anything (infinity and NaN too) is 0.
    float_binary(t, "v_mul_legacy_f32", "((A == 0.0f || B == 0.0f) ? 0.0f : A * B)");
    float_binary(t, "v_max_f32", "max(A, B)");
    float_binary(t, "v_min_f32", "min(A, B)");
    float_binary(t, "v_ldexp_f32", "ldexp(A, float(asint(B)))");
    float_unary(t, "v_rcp_f32", "rcp(X)");
    float_unary(t, "v_rcp_iflag_f32", "rcp(X)");
    float_unary(t, "v_rsq_f32", "rsqrt(X)");
    float_unary(t, "v_sqrt_f32", "sqrt(X)");
    float_unary(t, "v_exp_f32", "exp2(X)");
    float_unary(t, "v_log_f32", "log2(X)");
    // The hardware's sine and cosine take turns, not radians.
    float_unary(t, "v_sin_f32", "sin(X * 6.2831853f)");
    float_unary(t, "v_cos_f32", "cos(X * 6.2831853f)");
    float_unary(t, "v_fract_f32", "frac(X)");
    float_unary(t, "v_trunc_f32", "trunc(X)");
    float_unary(t, "v_floor_f32", "floor(X)");
    float_unary(t, "v_ceil_f32", "ceil(X)");
    float_unary(t, "v_rndne_f32", "round(X)");
    float_ternary(t, "v_fma_f32", "mad(A, B, C)");
    float_ternary(t, "v_mad_f32", "mad(A, B, C)");
    float_ternary(t, "v_fma_legacy_f32", "(((A == 0.0f || B == 0.0f) ? 0.0f : A * B) + C)");
    float_ternary(t, "v_max3_f32", "max(A, max(B, C))");
    float_ternary(t, "v_min3_f32", "min(A, min(B, C))");
    float_ternary(t, "v_med3_f32", "max(min(A, B), min(max(A, B), C))");
    // dst = A * B + dst.
    for (const char* name : {"v_fmac_f32", "v_mac_f32"}) {
      t[name] = [](Ctx& c) {
        if (!vector_form(c)) return false;
        const uint32_t d = vop_dst(c);
        c.vwrite(d, fresult(c, "mad(" + fsrc(c, 0) + ", " + fsrc(c, 1) + ", asfloat(" + c.vdst(d) + "))"));
        return true;
      };
    }
    // The literal is K: fmamk A * K + B, fmaak A * B + K.
    for (const auto& [name, mk] : {std::pair{"v_fmamk_f32", true}, {"v_madmk_f32", true}, {"v_fmaak_f32", false}, {"v_madak_f32", false}}) {
      t[name] = [mk = mk](Ctx& c) {
        const std::string a = fsrc(c, 0), b = fsrc(c, 1), k = "asfloat(" + hex32(c.in->literal()) + ")";
        c.vwrite(vop_dst(c), "asuint(" + (mk ? "mad(" + a + ", " + k + ", " + b + ")" : "mad(" + a + ", " + b + ", " + k + ")") + ")");
        return true;
      };
    }

    // -- 16-bit and packed --
    int16(t, "v_lshrrev_b16", "(B >> (A & 15u))");
    int16(t, "v_lshlrev_b16", "(B << (A & 15u))");
    int16(t, "v_ashrrev_i16", "asuint((asint(B << 16) >> 16) >> (A & 15u))");
    int16(t, "v_add_nc_u16", "(A + B)");
    int16(t, "v_sub_nc_u16", "(A - B)");
    int16(t, "v_mul_lo_u16", "(A * B)");
    int16(t, "v_max_u16", "max(A, B)");
    int16(t, "v_min_u16", "min(A, B)");
    int16(t, "v_max_i16", "asuint(max(asint(A << 16) >> 16, asint(B << 16) >> 16))");
    int16(t, "v_min_i16", "asuint(min(asint(A << 16) >> 16, asint(B << 16) >> 16))");
    float16(t, "v_add_f16", "(A + B)");
    float16(t, "v_sub_f16", "(A - B)");
    float16(t, "v_subrev_f16", "(B - A)");
    float16(t, "v_mul_f16", "(A * B)");
    float16(t, "v_max_f16", "max(A, B)");
    float16(t, "v_min_f16", "min(A, B)");
    float16(t, "v_min3_f16", "min(A, min(B, C))");
    float16(t, "v_max3_f16", "max(A, max(B, C))");
    float16(t, "v_med3_f16", "max(min(A, B), min(max(A, B), C))");
    float16(t, "v_fma_f16", "mad(A, B, C)");
    float16(t, "v_rcp_f16", "rcp(A)");
    float16(t, "v_sqrt_f16", "sqrt(A)");
    float16(t, "v_rsq_f16", "rsqrt(A)");
    float16(t, "v_floor_f16", "floor(A)");
    float16(t, "v_fract_f16", "frac(A)");
    t["v_fmac_f16"] = [](Ctx& c) {
      if (!form16(c)) return false;
      write16(c, f16_result(c, "mad(" + fsrc16(c, 0) + ", " + fsrc16(c, 1) + ", f16tof32(" + c.vdst(vop_dst(c)) + " & 0xFFFFu))"));
      return true;
    };
    packed16(t, "v_pk_add_f16", "(A + B)", true, 2);
    packed16(t, "v_pk_mul_f16", "(A * B)", true, 2);
    packed16(t, "v_pk_fma_f16", "mad(A, B, C)", true, 3);
    packed16(t, "v_pk_max_f16", "max(A, B)", true, 2);
    packed16(t, "v_pk_min_f16", "min(A, B)", true, 2);
    packed16(t, "v_pk_add_u16", "(A + B)", false, 2);
    packed16(t, "v_pk_sub_u16", "(A - B)", false, 2);
    packed16(t, "v_pk_mul_lo_u16", "(A * B)", false, 2);
    packed16(t, "v_pk_lshlrev_b16", "(B << (A & 15u))", false, 2);
    packed16(t, "v_pk_lshrrev_b16", "(B >> (A & 15u))", false, 2);

    // -- conversions --
    float_unary(t, "v_cvt_f32_u32", "float(asuint(X))");
    float_unary(t, "v_cvt_f32_i32", "float(asint(X))");
    for (int n = 0; n < 4; ++n) {
      t["v_cvt_f32_ubyte" + std::to_string(n)] = [n](Ctx& c) {
        if (!vector_form(c)) return false;
        c.vwrite(vop_dst(c), fresult(c, "float((" + vsrc(c, 0) + " >> " + std::to_string(8 * n) + "u) & 0xFFu)"));
        return true;
      };
    }
    int_op(t, "v_cvt_u32_f32", "am_cvt_u32(asfloat(A))");
    int_op(t, "v_cvt_i32_f32", "asuint(am_cvt_i32(asfloat(A)))");
    t["v_cvt_f32_f16"] = [](Ctx& c) {
      if (!vector_form(c)) return false;
      c.vwrite(vop_dst(c), fresult(c, "f16tof32(" + vsrc(c, 0, true) + ")"));
      return true;
    };
    t["v_cvt_f16_f32"] = [](Ctx& c) {
      if (!form16(c)) return false;
      write16(c, f16_result(c, fsrc(c, 0)));
      return true;
    };
    // 16-bit integers to halves.
    t["v_cvt_f16_u16"] = [](Ctx& c) {
      if (!form16(c)) return false;
      write16(c, f16_result(c, "float(" + src16(c, 0) + ")"));
      return true;
    };
    t["v_cvt_f16_i16"] = [](Ctx& c) {
      if (!form16(c)) return false;
      write16(c, f16_result(c, "float(asint(" + src16(c, 0) + " << 16u) >> 16)"));
      return true;
    };
    // Round toward zero: f32tof16 does that (on the GPUs tested); am_f16 rounds to nearest even.
    int_op(t, "v_cvt_pkrtz_f16_f32", "(f32tof16(asfloat(A)) | (f32tof16(asfloat(B)) << 16))");
    int_op(t, "v_pack_b32_f16", "((A & 0xFFFFu) | (B << 16))");
    int_op(t, "v_cvt_pk_u8_f32", "am_cvt_pk_u8(asfloat(A), B, C)");

    // -- vector integers and bits --
    int_op(t, "v_mov_b32", "A");
    int_op(t, "v_and_b32", "(A & B)");
    int_op(t, "v_or_b32", "(A | B)");
    int_op(t, "v_xor_b32", "(A ^ B)");
    int_op(t, "v_not_b32", "(~A)");
    int_op(t, "v_add_nc_u32", "(A + B)");
    int_op(t, "v_sub_nc_u32", "(A - B)");
    int_op(t, "v_subrev_nc_u32", "(B - A)");
    int_op(t, "v_lshlrev_b32", "(B << (A & 31u))");
    int_op(t, "v_lshrrev_b32", "(B >> (A & 31u))");
    int_op(t, "v_ashrrev_i32", "asuint(asint(B) >> (A & 31u))");
    int_op(t, "v_max_u32", "max(A, B)");
    int_op(t, "v_min_u32", "min(A, B)");
    int_op(t, "v_max_i32", "asuint(max(asint(A), asint(B)))");
    int_op(t, "v_min_i32", "asuint(min(asint(A), asint(B)))");
    int_op(t, "v_mul_lo_u32", "(A * B)");
    int_op(t, "v_mul_hi_u32", "uint((uint64_t(A) * uint64_t(B)) >> 32)");
    int_op(t, "v_mul_hi_i32", "uint(uint64_t(int64_t(asint(A)) * int64_t(asint(B))) >> 32)");
    int_op(t, "v_mul_u32_u24", "((A & 0xFFFFFFu) * (B & 0xFFFFFFu))");
    int_op(t, "v_mad_u32_u24", "((A & 0xFFFFFFu) * (B & 0xFFFFFFu) + C)");
    int_op(t, "v_mul_i32_i24", "asuint((asint(A << 8) >> 8) * (asint(B << 8) >> 8))");
    int_op(t, "v_mul_hi_i32_i24", "uint(uint64_t(int64_t(asint(A << 8) >> 8) * int64_t(asint(B << 8) >> 8)) >> 32)");
    int_op(t, "v_mul_hi_u32_u24", "uint((uint64_t(A & 0xFFFFFFu) * uint64_t(B & 0xFFFFFFu)) >> 32)");
    int_op(t, "v_mad_i32_i24", "asuint((asint(A << 8) >> 8) * (asint(B << 8) >> 8) + asint(C))");
    int_op(t, "v_bfe_u32", "am_bfe_u32(A, B, C)");
    int_op(t, "v_bfm_b32", "(((1u << (A & 31u)) - 1u) << (B & 31u))");
    int_op(t, "v_bfe_i32", "am_bfe_i32(A, B, C)");
    int_op(t, "v_bfi_b32", "((A & B) | (~A & C))");
    int_op(t, "v_lshl_add_u32", "((A << (B & 31u)) + C)");
    int_op(t, "v_add_lshl_u32", "((A + B) << (C & 31u))");
    int_op(t, "v_lshl_or_b32", "((A << (B & 31u)) | C)");
    int_op(t, "v_and_or_b32", "((A & B) | C)");
    int_op(t, "v_or3_b32", "(A | B | C)");
    int_op(t, "v_add3_u32", "(A + B + C)");
    int_op(t, "v_xad_u32", "((A ^ B) + C)");
    int_op(t, "v_med3_i32", "asuint(max(min(asint(A), asint(B)), min(max(asint(A), asint(B)), asint(C))))");
    int_op(t, "v_med3_u32", "max(min(A, B), min(max(A, B), C))");
    int_op(t, "v_max3_u32", "max(A, max(B, C))");
    int_op(t, "v_min3_u32", "min(A, min(B, C))");
    int_op(t, "v_perm_b32", "am_perm(A, B, C)");
    int_op(t, "v_bfrev_b32", "reversebits(A)");
    int_op(t, "v_ffbl_b32", "firstbitlow(A)");
    int_op(t, "v_bcnt_u32_b32", "(countbits(A) + B)");
    int_op(t, "v_mbcnt_lo_u32_b32", "(countbits(A & am_lanes_below_lo()) + B)");
    int_op(t, "v_mbcnt_hi_u32_b32", "(countbits(A & am_lanes_below_hi()) + B)");
    int_op(t, "v_alignbit_b32", "uint(am_u64(B, A) >> (C & 31u))");
    int_op(t, "v_alignbyte_b32", "uint(am_u64(B, A) >> ((C & 3u) * 8u))");
    int_op(t, "v_ffbh_u32", "(A == 0u ? 0xFFFFFFFFu : 31u - firstbithigh(A))");
    // The first bit from the top that differs from the sign; none (0 or -1): all ones.
    int_op(t, "v_ffbh_i32", "((asint(A) == 0 || asint(A) == -1) ? 0xFFFFFFFFu : 31u - firstbithigh(asint(A)))");
    int_op(t, "v_xnor_b32", "(~(A ^ B))");
    int_op(t, "v_cvt_floor_i32_f32", "asuint(am_cvt_i32(floor(asfloat(A))))");
    int_op(t, "v_cvt_rpi_i32_f32", "asuint(am_cvt_i32(floor(asfloat(A) + 0.5f)))");
    // Cube maps: face id, the face's s and t, and twice the major axis.
    float_ternary(t, "v_cubeid_f32", "am_cube(A, B, C).x");
    float_ternary(t, "v_cubesc_f32", "am_cube(A, B, C).y");
    float_ternary(t, "v_cubetc_f32", "am_cube(A, B, C).z");
    float_ternary(t, "v_cubema_f32", "am_cube(A, B, C).w");
    // Mixed precision: each source is f32, or with op_sel_hi a half (op_sel: the high one);
    // NEG_HI is abs here. mixlo / mixhi round the result to a half in that half of the register.
    for (const auto& [name, to] : {std::pair{"v_fma_mix_f32", 0}, {"v_fma_mixlo_f16", 1}, {"v_fma_mixhi_f16", 2}}) {
      t[name] = [to = to](Ctx& c) {
        const uint32_t sel = c.f(Field::OP_SEL), hi = c.f(Field::OP_SEL_HI) | c.f(Field::OP_SEL_HI_2) << 2;
        const uint32_t neg = c.f(Field::NEG), abs = c.f(Field::NEG_HI);
        std::string x[3];
        for (int k = 0; k < 3; ++k) {
          const bool half = (hi >> k) & 1;
          const std::string r = vsrc(c, k, half);
          x[k] = half ? "f16tof32((" + r + " >> " + ((sel >> k) & 1 ? "16u" : "0u") + ") & 0xFFFFu)" : "asfloat(" + r + ")";
          if ((abs >> k) & 1) x[k] = "abs(" + x[k] + ")";
          if ((neg >> k) & 1) x[k] = "(-" + x[k] + ")";
        }
        std::string r = "mad(" + x[0] + ", " + x[1] + ", " + x[2] + ")";
        if (c.f(Field::CLAMP)) r = "saturate(" + r + ")";
        const uint32_t d = vop_dst(c);
        if (to == 0) c.vwrite(d, "asuint(" + r + ")");
        else if (to == 1) c.vwrite(d, "((" + c.vdst(d) + " & 0xFFFF0000u) | am_f16(" + r + "))");
        else c.vwrite(d, "((" + c.vdst(d) + " & 0xFFFFu) | (am_f16(" + r + ") << 16))");
        return true;
      };
    }
    // Lane permutes: lane l reads src0 from the lane src1/src2 name for it (four bits a lane), in
    // its own row (permlane16) or the other row of its half (permlanex16). op_sel: FI, BOUND_CTRL.
    for (const bool cross : {false, true}) {
      t[cross ? "v_permlanex16_b32" : "v_permlane16_b32"] = [cross](Ctx& c) {
        const uint32_t r = c.f(Field::SRC0) - 256, d = vop_dst(c), sel = c.f(Field::OP_SEL);
        if (c.f(Field::SRC0) < 256) return c.fail("permlane source");
        c.touch_v(r);
        const std::string s1 = c.src(c.f(Field::SRC1)), s2 = c.src(c.f(Field::SRC2));
        c.line("{ uint i = am_lane & 15u; uint k = i < 8u ? (" + s1 + " >> (i * 4u)) & 15u : (" + s2 + " >> ((i - 8u) * 4u)) & 15u;");
        c.line(std::string("  uint l = ") + (cross ? "((am_lane & ~15u) ^ 16u)" : "(am_lane & ~15u)") + " | k;");
        c.line("  uint x = WaveReadLaneAt(v[" + num(r) + "], l);");
        c.line(std::string("  bool ok = ") + ((sel & 1) ? "true" : "am_lane_on(l)") + ";");
        c.line("  if (AM_ACT) { if (ok) " + c.vdst(d) + " = x;" + ((sel & 2) ? " else " + c.vdst(d) + " = 0u;" : "") + " } }");
        return true;
      };
    }
    t["v_nop"] = [](Ctx&) { return true; };
    t["v_lshlrev_b64"] = [](Ctx& c) {
      if (!plain_form(c)) return false;
      const uint32_t d = vop_dst(c);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint64_t r = " + c.src64(c.f(Field::SRC1)) + " << (" + vsrc(c, 0) + " & 63u); v[" + num(d) +
             "] = uint(r); v[" + num(d + 1) + "] = uint(r >> 32); }");
      return true;
    };
    t["v_lshrrev_b64"] = [](Ctx& c) {
      if (!plain_form(c)) return false;
      const uint32_t d = vop_dst(c);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint64_t r = " + c.src64(c.f(Field::SRC1)) + " >> (" + vsrc(c, 0) + " & 63u); v[" + num(d) +
             "] = uint(r); v[" + num(d + 1) + "] = uint(r >> 32); }");
      return true;
    };
    // VOP2: VCC picks src1; VOP3: the mask in src2.
    t["v_cndmask_b32"] = [](Ctx& c) {
      if (!vector_form(c)) return false;
      std::string mask;
      if (is_vop3(*c.in)) {
        const uint32_t m = c.f(Field::SRC2);
        mask = c.wave64 ? "am_lane_bit(s[" + num(m) + "], s[" + num(m + 1) + "])" : "am_lane_bit(s[" + num(m) + "], 0u)";
        if (m > 107 && m != 126) return c.fail("v_cndmask_b32 mask " + num(m));
        c.vwrite(vop_dst(c), "(" + mask + " ? asuint(" + fsrc(c, 1) + ") : asuint(" + fsrc(c, 0) + "))");
      } else {
        mask = c.wave64 ? "am_lane_bit(s[106], s[107])" : "am_lane_bit(s[106], 0u)";
        c.vwrite(vop_dst(c), "(" + mask + " ? " + vsrc(c, 1) + " : " + vsrc(c, 0) + ")");
      }
      return true;
    };
    // Carry: VOP3b writes the carry mask to its SDST; VOP2 reads and writes VCC.
    t["v_add_co_u32"] = [](Ctx& c) {
      if (!plain_form(c) || !is_vop3(*c.in)) return c.fail("v_add_co_u32 form");
      const uint32_t sd = c.f(Field::SDST);
      c.line("{ uint a = " + vsrc(c, 0) + ", b = " + vsrc(c, 1) + "; uint r = a + b; bool cy = r < a;");
      c.line("  uint4 m = WaveActiveBallot(cy && AM_ACT != 0u);");
      c.line("  if (AM_ACT) " + c.vdst(vop_dst(c)) + " = r;");
      if (sd != 125) c.line("  s[" + num(sd) + "] = m.x;" + (c.wave64 ? " s[" + num(sd + 1) + "] = m.y;" : std::string()));
      c.line("}");
      return true;
    };
    // VOP3B: D (64 bits) = S0 * S1 + S2 (64 bits); SDST gets each lane's carry out of the add.
    t["v_mad_u64_u32"] = [](Ctx& c) {
      if (!plain_form(c) || !is_vop3(*c.in)) return c.fail("v_mad_u64_u32 form");
      const uint32_t sd = c.f(Field::SDST), d = vop_dst(c);
      c.touch_v(d + 1);
      c.line("{ uint64_t a = " + c.src64(c.f(Field::SRC2)) + "; uint64_t r = uint64_t(" + vsrc(c, 0) + ") * uint64_t(" + vsrc(c, 1) +
             ") + a; bool cy = r < a;");
      c.line("  uint4 m = WaveActiveBallot(cy && AM_ACT != 0u);");
      c.line("  if (AM_ACT) { v[" + num(d) + "] = uint(r); v[" + num(d + 1) + "] = uint(r >> 32); }");
      if (sd != 125) c.line("  s[" + num(sd) + "] = m.x;" + (c.wave64 ? " s[" + num(sd + 1) + "] = m.y;" : std::string()));
      c.line("}");
      return true;
    };
    t["v_add_co_ci_u32"] = [](Ctx& c) {
      if (!plain_form(c)) return false;
      const bool vop3 = is_vop3(*c.in);
      const uint32_t in = vop3 ? c.f(Field::SRC2) : 106, out = vop3 ? c.f(Field::SDST) : 106;
      c.line("{ uint a = " + vsrc(c, 0) + ", b = " + vsrc(c, 1) + "; uint ci = am_lane_bit(s[" + num(in) + "], " +
             (c.wave64 ? "s[" + num(in + 1) + "]" : std::string("0u")) + ") ? 1u : 0u;");
      c.line("  uint r = a + b + ci; bool cy = (uint64_t(a) + b + ci) > 0xFFFFFFFFull;");
      c.line("  uint4 m = WaveActiveBallot(cy && AM_ACT != 0u);");
      c.line("  if (AM_ACT) " + c.vdst(vop_dst(c)) + " = r;");
      if (out != 125) c.line("  s[" + num(out) + "] = m.x;" + (c.wave64 ? " s[" + num(out + 1) + "] = m.y;" : std::string()));
      c.line("}");
      return true;
    };

    // -- compares --
    struct Cmp {
      const char* name;
      const char* cond;
    };
    const Cmp fcmp[] = {{"f", "false"},        {"lt", "A < B"},  {"eq", "A == B"}, {"le", "A <= B"},
                        {"gt", "A > B"},       {"lg", "(A < B || A > B)"}, {"ge", "A >= B"},
                        {"o", "(A == A && B == B)"}, {"u", "(A != A || B != B)"}, {"nge", "!(A >= B)"},
                        {"nlg", "!(A < B || A > B)"}, {"ngt", "!(A > B)"}, {"nle", "!(A <= B)"}, {"neq", "!(A == B)"},
                        {"nlt", "!(A < B)"}, {"tru", "true"}};
    for (const Cmp& x : fcmp) {
      compare(t, std::string(x.name) + "_f32", x.cond, true);
      compare(t, std::string(x.name) + "_f16", x.cond, true, false, true);
    }
    const Cmp icmp[] = {{"f", "false"}, {"lt", "A < B"}, {"eq", "A == B"}, {"le", "A <= B"}, {"gt", "A > B"},
                        {"ne", "A != B"}, {"ge", "A >= B"}, {"t", "true"}};
    for (const Cmp& x : icmp) {
      compare(t, std::string(x.name) + "_u32", x.cond, false);
      compare(t, std::string(x.name) + "_i32", fill(x.cond, "asint(A)", "asint(B)"), false);
      compare(t, std::string(x.name) + "_u64", x.cond, false, true);
      // 16-bit integer compares: the low halves (op_sel's high halves are not read yet).
      compare(t, std::string(x.name) + "_u16", fill(x.cond, "(A & 0xFFFFu)", "(B & 0xFFFFu)"), false);
      compare(t, std::string(x.name) + "_i16", fill(x.cond, "(asint(A << 16) >> 16)", "(asint(B << 16) >> 16)"), false);
    }
    compare(t, "class_f32", "am_class(A, asuint(B))", false);

    // -- lanes --
    t["v_readfirstlane_b32"] = [](Ctx& c) {
      c.line(c.sdst(c.f(Field::VDST)) + " = WaveReadLaneAt(" + vsrc(c, 0) + ", am_first_active());");
      return true;
    };
    t["v_readlane_b32"] = [](Ctx& c) {
      c.line(c.sdst(c.f(Field::VDST)) + " = WaveReadLaneAt(" + vsrc(c, 0) + ", " + vsrc(c, 1) + " & " +
             (c.wave64 ? "63u" : "31u") + ");");
      return true;
    };
    t["v_writelane_b32"] = [](Ctx& c) {
      c.line("if (am_lane == (" + vsrc(c, 1) + " & " + (c.wave64 ? "63u" : "31u") + ")) " + c.vdst(vop_dst(c)) + " = " +
             vsrc(c, 0) + ";");
      return true;
    };

    // -- scalar ALU --
    struct S2 {
      const char* name;
      const char* expr;  // A, B: the sources; the result r
      const char* scc;   // with r, a, b; "" keeps SCC
    };
    const S2 sop2[] = {
        {"s_add_u32", "A + B", "r < a"},
        {"s_sub_u32", "A - B", "b > a"},
        {"s_addc_u32", "A + B + (scc ? 1u : 0u)", "(uint64_t(a) + b + (scc ? 1u : 0u)) > 0xFFFFFFFFull"},
        {"s_subb_u32", "A - B - (scc ? 1u : 0u)", "(uint64_t(b) + (scc ? 1u : 0u)) > a"},
        {"s_add_i32", "A + B", "((~(a ^ b)) & (a ^ r)) >> 31 != 0"},
        {"s_sub_i32", "A - B", "((a ^ b) & (a ^ r)) >> 31 != 0"},
        {"s_and_b32", "A & B", "r != 0u"},
        {"s_or_b32", "A | B", "r != 0u"},
        {"s_xor_b32", "A ^ B", "r != 0u"},
        {"s_andn2_b32", "A & ~B", "r != 0u"},
        {"s_orn2_b32", "A | ~B", "r != 0u"},
        {"s_nand_b32", "~(A & B)", "r != 0u"},
        {"s_nor_b32", "~(A | B)", "r != 0u"},
        {"s_xnor_b32", "~(A ^ B)", "r != 0u"},
        {"s_lshl_b32", "A << (B & 31u)", "r != 0u"},
        {"s_lshr_b32", "A >> (B & 31u)", "r != 0u"},
        {"s_ashr_i32", "asuint(asint(A) >> (B & 31u))", "r != 0u"},
        {"s_min_u32", "min(A, B)", "r == a"},
        {"s_max_u32", "max(A, B)", "r == a"},
        {"s_min_i32", "asuint(min(asint(A), asint(B)))", "r == a"},
        {"s_max_i32", "asuint(max(asint(A), asint(B)))", "r == a"},
        {"s_mul_i32", "A * B", ""},
        {"s_mul_hi_u32", "uint((uint64_t(A) * uint64_t(B)) >> 32)", ""},
        {"s_bfe_u32", "am_bfe_u32(A, B & 31u, (B >> 16) & 0x7Fu)", "r != 0u"},
        {"s_bfe_i32", "am_bfe_i32(A, B & 31u, (B >> 16) & 0x7Fu)", "r != 0u"},
        {"s_bfm_b32", "((1u << (A & 31u)) - 1u) << (B & 31u)", ""},
        {"s_cselect_b32", "scc ? A : B", ""},
        {"s_lshl1_add_u32", "(A << 1) + B", "r < (a << 1)"},
        {"s_lshl2_add_u32", "(A << 2) + B", "r < (a << 2)"},
        {"s_pack_ll_b32_b16", "(A & 0xFFFFu) | (B << 16)", ""},
    };
    for (const S2& x : sop2) {
      t[x.name] = [x](Ctx& c) {
        const std::string e = fill(x.expr, "a", "b");
        c.line("{ uint a = " + c.src(c.f(Field::SSRC0)) + ", b = " + c.src(c.f(Field::SSRC1)) + "; uint r = " + e + ";" +
               (std::string(x.scc).empty() ? "" : " scc = " + std::string(x.scc) + ";") + " " + c.sdst(c.f(Field::SDST)) +
               " = r; }");
        return true;
      };
    }
    const S2 sop2_64[] = {
        {"s_and_b64", "a & b", "r != 0"},   {"s_or_b64", "a | b", "r != 0"},     {"s_xor_b64", "a ^ b", "r != 0"},
        {"s_andn2_b64", "a & ~b", "r != 0"}, {"s_orn2_b64", "a | ~b", "r != 0"}, {"s_nand_b64", "~(a & b)", "r != 0"},
        {"s_nor_b64", "~(a | b)", "r != 0"}, {"s_xnor_b64", "~(a ^ b)", "r != 0"}, {"s_cselect_b64", "scc ? a : b", ""},
    };
    for (const S2& x : sop2_64) {
      t[x.name] = [x](Ctx& c) {
        c.line("{ uint64_t a = " + c.src64(c.f(Field::SSRC0)) + ", b = " + c.src64(c.f(Field::SSRC1)) + "; uint64_t r = " +
               x.expr + ";" + (std::string(x.scc).empty() ? "" : " scc = " + std::string(x.scc) + ";"));
        const uint32_t d = c.f(Field::SDST);
        if (d != 125) c.line("  s[" + num(d) + "] = uint(r); s[" + num(d + 1) + "] = uint(r >> 32);");
        c.line("}");
        return true;
      };
    }
    for (const auto& [name, op] : {std::pair{"s_lshl_b64", "<<"}, {"s_lshr_b64", ">>"}}) {
      t[name] = [op = std::string(op)](Ctx& c) {
        c.line("{ uint64_t r = " + c.src64(c.f(Field::SSRC0)) + " " + op + " (" + c.src(c.f(Field::SSRC1)) + " & 63u); scc = r != 0;");
        c.line("  s[" + num(c.f(Field::SDST)) + "] = uint(r); s[" + num(c.f(Field::SDST) + 1) + "] = uint(r >> 32); }");
        return true;
      };
    }
    struct S1 {
      const char* name;
      const char* expr;
      const char* scc;
    };
    const S1 sop1[] = {
        {"s_mov_b32", "a", ""},          {"s_not_b32", "~a", "r != 0u"}, {"s_brev_b32", "reversebits(a)", ""},
        {"s_bcnt1_i32_b32", "countbits(a)", "r != 0u"}, {"s_ff1_i32_b32", "firstbitlow(a)", ""},
        {"s_wqm_b32", "am_wqm(a)", "r != 0u"},       {"s_sext_i32_i8", "asuint(int(a << 24) >> 24)", ""},
        {"s_sext_i32_i16", "asuint(int(a << 16) >> 16)", ""}, {"s_abs_i32", "asuint(abs(asint(a)))", "r != 0u"},
        {"s_bcnt0_i32_b32", "countbits(~a)", "r != 0u"}, {"s_ff0_i32_b32", "firstbitlow(~a)", ""},
        // Leading zeros from the top bit; -1 for zero.
        {"s_flbit_i32_b32", "(a == 0u ? 0xFFFFFFFFu : 31u - firstbithigh(a))", ""},
    };
    for (const S1& x : sop1) {
      t[x.name] = [x](Ctx& c) {
        c.line("{ uint a = " + c.src(c.f(Field::SSRC0)) + "; uint r = " + x.expr + ";" +
               (std::string(x.scc).empty() ? "" : " scc = " + std::string(x.scc) + ";") + " " + c.sdst(c.f(Field::SDST)) +
               " = r; }");
        return true;
      };
    }
    // 64-bit sources, 32-bit results.
    const S1 sop1_64to32[] = {
        {"s_bcnt1_i32_b64", "countbits(uint(a)) + countbits(uint(a >> 32))", "r != 0u"},
        {"s_bcnt0_i32_b64", "countbits(~uint(a)) + countbits(~uint(a >> 32))", "r != 0u"},
        {"s_ff1_i32_b64", "(uint(a) != 0u ? firstbitlow(uint(a)) : uint(a >> 32) != 0u ? 32u + firstbitlow(uint(a >> 32)) : 0xFFFFFFFFu)", ""},
        {"s_ff0_i32_b64", "(~uint(a) != 0u ? firstbitlow(~uint(a)) : ~uint(a >> 32) != 0u ? 32u + firstbitlow(~uint(a >> 32)) : 0xFFFFFFFFu)", ""},
    };
    for (const S1& x : sop1_64to32) {
      t[x.name] = [x](Ctx& c) {
        c.line("{ uint64_t a = " + c.src64(c.f(Field::SSRC0)) + "; uint r = " + x.expr + ";" +
               (std::string(x.scc).empty() ? "" : " scc = " + std::string(x.scc) + ";") + " " + c.sdst(c.f(Field::SDST)) +
               " = r; }");
        return true;
      };
    }
    // Set or clear bit s0 of the destination itself.
    for (const bool one : {false, true}) {
      t[one ? "s_bitset1_b32" : "s_bitset0_b32"] = [one](Ctx& c) {
        const std::string d = c.sdst(c.f(Field::SDST));
        c.line(d + (one ? " |= " : " &= ~") + "(1u << (" + c.src(c.f(Field::SSRC0)) + " & 31u));");
        return true;
      };
      t[one ? "s_bitset1_b64" : "s_bitset0_b64"] = [one](Ctx& c) {
        const uint32_t d = c.f(Field::SDST);
        if (d > 105 && d != 106 && d != 126) return c.fail("64-bit scalar destination " + num(d));
        c.line("{ uint b = " + c.src(c.f(Field::SSRC0)) + " & 63u; uint i = " + num(d) + "u + (b >> 5);");
        c.line(std::string("  s[i] ") + (one ? "|= " : "&= ~") + "(1u << (b & 31u)); }");
        return true;
      };
    }
    t["s_mov_b64"] = [](Ctx& c) {
      c.set_s64(c.f(Field::SDST), c.src64(c.f(Field::SSRC0)));
      return true;
    };
    t["s_not_b64"] = [](Ctx& c) {
      c.line("{ uint64_t r = ~" + c.src64(c.f(Field::SSRC0)) + "; scc = r != 0; s[" + num(c.f(Field::SDST)) +
             "] = uint(r); s[" + num(c.f(Field::SDST) + 1) + "] = uint(r >> 32); }");
      return true;
    };
    t["s_wqm_b64"] = [](Ctx& c) {
      c.line("{ uint64_t a = " + c.src64(c.f(Field::SSRC0)) + "; uint lo = am_wqm(uint(a)), hi = am_wqm(uint(a >> 32)); scc = (lo | hi) != 0u; s[" +
             num(c.f(Field::SDST)) + "] = lo; s[" + num(c.f(Field::SDST) + 1) + "] = hi; }");
      return true;
    };
    // EXEC: d = exec; exec = op(s0, exec); scc = exec != 0. Wave64 on _b64, wave32 on _b32.
    struct Sx {
      const char* name;
      const char* expr;  // with a (s0) and e (exec)
      bool wrexec;       // the _wrexec forms write the new EXEC to d, not the old
    };
    const Sx saveexec[] = {{"and", "a & e", false}, {"or", "a | e", false}, {"xor", "a ^ e", false},
                           {"andn2", "a & ~e", false}, {"orn2", "a | ~e", false}, {"nand", "~(a & e)", false},
                           {"nor", "~(a | e)", false}, {"xnor", "~(a ^ e)", false}, {"andn1", "~a & e", false},
                           {"orn1", "~a | e", false}};
    for (const Sx& x : saveexec) {
      t[std::string("s_") + x.name + "_saveexec_b32"] = [x](Ctx& c) {
        c.line("{ uint a = " + c.src(c.f(Field::SSRC0)) + ", e = s[126]; uint r = " + x.expr + "; " + c.sdst(c.f(Field::SDST)) +
               " = e; s[126] = r; scc = r != 0u; }");
        return true;
      };
      t[std::string("s_") + x.name + "_saveexec_b64"] = [x](Ctx& c) {
        c.line("{ uint64_t a = " + c.src64(c.f(Field::SSRC0)) + ", e = am_u64(s[126], s[127]); uint64_t r = " + x.expr + ";");
        if (c.f(Field::SDST) != 125) c.line("  s[" + num(c.f(Field::SDST)) + "] = uint(e); s[" + num(c.f(Field::SDST) + 1) + "] = uint(e >> 32);");
        c.line("  s[126] = uint(r); s[127] = uint(r >> 32); scc = r != 0; }");
        return true;
      };
    }
    for (const auto& [name, expr] : {std::pair{"andn1", "~a & e"}, {"andn2", "a & ~e"}}) {
      t[std::string("s_") + name + "_wrexec_b32"] = [expr = std::string(expr)](Ctx& c) {
        c.line("{ uint a = " + c.src(c.f(Field::SSRC0)) + ", e = s[126]; uint r = " + expr + "; s[126] = r; " +
               c.sdst(c.f(Field::SDST)) + " = r; scc = r != 0u; }");
        return true;
      };
      t[std::string("s_") + name + "_wrexec_b64"] = [expr = std::string(expr)](Ctx& c) {
        c.line("{ uint64_t a = " + c.src64(c.f(Field::SSRC0)) + ", e = am_u64(s[126], s[127]); uint64_t r = " + expr +
               "; s[126] = uint(r); s[127] = uint(r >> 32); scc = r != 0;");
        if (c.f(Field::SDST) != 125) c.line("  s[" + num(c.f(Field::SDST)) + "] = uint(r); s[" + num(c.f(Field::SDST) + 1) + "] = uint(r >> 32);");
        c.line("}");
        return true;
      };
    }
    struct Sc {
      const char* name;
      const char* cond;
    };
    const Sc sopc[] = {{"eq_u32", "a == b"}, {"lg_u32", "a != b"}, {"gt_u32", "a > b"}, {"ge_u32", "a >= b"},
                       {"lt_u32", "a < b"},  {"le_u32", "a <= b"}, {"eq_i32", "a == b"}, {"lg_i32", "a != b"},
                       {"gt_i32", "asint(a) > asint(b)"}, {"ge_i32", "asint(a) >= asint(b)"},
                       {"lt_i32", "asint(a) < asint(b)"}, {"le_i32", "asint(a) <= asint(b)"}};
    for (const Sc& x : sopc) {
      t[std::string("s_cmp_") + x.name] = [x](Ctx& c) {
        c.line("{ uint a = " + c.src(c.f(Field::SSRC0)) + ", b = " + c.src(c.f(Field::SSRC1)) + "; scc = " + x.cond + "; }");
        return true;
      };
      // SOPK: the register against the 16-bit immediate (sign- or zero-extended).
      const bool is_signed = std::string_view(x.name).ends_with("i32");
      t[std::string("s_cmpk_") + x.name] = [x, is_signed](Ctx& c) {
        const uint32_t imm = c.f(Field::SIMM16);
        const uint32_t k = is_signed ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm))) : imm;
        c.line("{ uint a = " + c.src(c.f(Field::SDST)) + ", b = " + hex32(k) + "; scc = " + x.cond + "; }");
        return true;
      };
    }
    t["s_cmp_eq_u64"] = [](Ctx& c) {
      c.line("scc = " + c.src64(c.f(Field::SSRC0)) + " == " + c.src64(c.f(Field::SSRC1)) + ";");
      return true;
    };
    t["s_cmp_lg_u64"] = [](Ctx& c) {
      c.line("scc = " + c.src64(c.f(Field::SSRC0)) + " != " + c.src64(c.f(Field::SSRC1)) + ";");
      return true;
    };
    t["s_bitcmp0_b32"] = [](Ctx& c) {
      c.line("scc = ((" + c.src(c.f(Field::SSRC0)) + " >> (" + c.src(c.f(Field::SSRC1)) + " & 31u)) & 1u) == 0u;");
      return true;
    };
    t["s_bitcmp1_b32"] = [](Ctx& c) {
      c.line("scc = ((" + c.src(c.f(Field::SSRC0)) + " >> (" + c.src(c.f(Field::SSRC1)) + " & 31u)) & 1u) != 0u;");
      return true;
    };
    auto simm = [](Ctx& c) { return static_cast<int32_t>(static_cast<int16_t>(c.f(Field::SIMM16))); };
    t["s_movk_i32"] = [simm](Ctx& c) {
      c.line(c.sdst(c.f(Field::SDST)) + " = " + hex32(static_cast<uint32_t>(simm(c))) + ";");
      return true;
    };
    t["s_cmovk_i32"] = [simm](Ctx& c) {
      c.line("if (scc) " + c.sdst(c.f(Field::SDST)) + " = " + hex32(static_cast<uint32_t>(simm(c))) + ";");
      return true;
    };
    t["s_addk_i32"] = [simm](Ctx& c) {
      const std::string d = c.sdst(c.f(Field::SDST));
      c.line("{ uint a = " + d + ", b = " + hex32(static_cast<uint32_t>(simm(c))) + "; uint r = a + b; scc = ((~(a ^ b)) & (a ^ r)) >> 31 != 0; " + d + " = r; }");
      return true;
    };
    t["s_mulk_i32"] = [simm](Ctx& c) {
      const std::string d = c.sdst(c.f(Field::SDST));
      c.line(d + " = " + d + " * " + hex32(static_cast<uint32_t>(simm(c))) + ";");
      return true;
    };
    // Mode registers (rounding, denormals): the host keeps its own; nothing to do.
    t["s_setreg_imm32_b32"] = [](Ctx&) { return true; };
    t["s_setreg_b32"] = [](Ctx&) { return true; };
    t["s_getreg_b32"] = [](Ctx& c) {
      c.line(c.sdst(c.f(Field::SDST)) + " = 0u;");
      return true;
    };
    // Program counters: our code starts at address 0.
    t["s_getpc_b64"] = [](Ctx& c) {
      c.set_s64(c.f(Field::SDST), "uint64_t(" + num(uint64_t{c.next} * 4) + ")");
      return true;
    };
    for (const char* name : {"s_waitcnt", "s_nop", "s_inst_prefetch", "s_setprio", "s_waitcnt_vscnt", "s_waitcnt_lgkmcnt",
                             "s_waitcnt_vmcnt", "s_waitcnt_expcnt", "s_sleep", "s_icache_inv", "s_dcache_inv", "s_ttracedata",
                             "s_denorm_mode", "s_round_mode", "s_clause"}) {
      t[name] = [](Ctx&) { return true; };
    }
    // -- graphics: exports and attribute interpolation --
    // exp: position 0 (12) to SV_Position, parameter N (32 + N) to output N; render target N
    // (0-7) to SV_Target, two halves a register with COMPR. Other positions and null (9) are
    // dropped. Lanes whose EXEC bit is clear export nothing.
    t["exp"] = [](Ctx& c) {
      const uint32_t tgt = c.f(Field::TGT), en = c.f(Field::EN), compr = c.f(Field::COMPR);
      const uint32_t src[4] = {c.f(Field::VSRC0), c.f(Field::VSRC1), c.f(Field::VSRC2), c.f(Field::VSRC3)};
      static const char* const kChan[] = {"x", "y", "z", "w"};
      const HwStage stage = c.input->stage;
      std::string field;
      if (stage == HwStage::Vs && tgt == 12) field = "am_o.pos";
      else if (stage == HwStage::Vs && tgt >= 32 && tgt < 64 && tgt - 32 < c.input->outputs.size()) field = "am_o.o" + num(tgt - 32);
      else if (stage == HwStage::Ps && tgt < 8 && tgt < c.input->outputs.size()) field = "am_o.o" + num(tgt);
      else if (stage == HwStage::Ps && tgt == 8) {
        // MRTZ: depth in channel x (the 32-bit Z formats). Stencil (y), the sample mask (z) and
        // alpha-to-coverage (w) are not passed on.
        if (compr) return c.fail("16-bit depth export");
        if (!(en & 1u)) return true;
        c.depth_export = true;
        c.touch_v(src[0]);
        c.line("if (AM_ACT != 0u) { am_o.am_depth = asfloat(v[" + num(src[0]) + "]); }");
        return true;
      }
      else if (tgt == 9 || (stage == HwStage::Vs && ((tgt >= 13 && tgt < 16) || tgt >= 32))) return true;
      else return c.fail("exp to target " + num(tgt));
      std::string body;
      for (uint32_t k = 0; k < 4; ++k) {
        if (!((en >> k) & 1u) && !(compr && stage == HwStage::Vs && k < 2)) continue;
        std::string value;
        if (compr && stage == HwStage::Ps) {
          const uint32_t r = src[k / 2];
          c.touch_v(r);
          value = "f16tof32(v[" + num(r) + "]" + (k % 2 ? " >> 16" : "") + ")";
        } else if (compr) {
          // A vertex shader's 16-bit parameters, two halves a register: VSRC0 holds channels
          // 0 and 1, VSRC1 channels 2 and 3. The attribute's dwords x and y carry them as bits;
          // the pixel shader's 16-bit interpolation unpacks them (v_interp_p1ll_f16).
          if (k >= 2) continue;
          if (!((en >> (2 * k)) & 3u)) continue;
          c.touch_v(src[k]);
          value = "asfloat(v[" + num(src[k]) + "])";
        } else {
          c.touch_v(src[k]);
          value = "asfloat(v[" + num(src[k]) + "])";
        }
        body += " " + field + "." + kChan[k] + " = " + value + ";";
      }
      if (!body.empty()) c.line("if (AM_ACT != 0u) {" + body + " }");
      return true;
    };
    // v_interp_p1 / p2 / mov: the host interpolates; each gives attribute ATTR's channel.
    for (const char* name : {"v_interp_p1_f32", "v_interp_p2_f32", "v_interp_mov_f32"}) {
      t[name] = [](Ctx& c) {
        if (c.in->encoding != rd::Encoding::Vintrp || c.input->stage != HwStage::Ps) return c.fail("v_interp outside a pixel shader's VINTRP form");
        const uint32_t attr = c.f(Field::ATTR), chan = c.f(Field::ATTRCHAN);
        if (attr >= c.input->inputs.size()) return c.fail("v_interp of attribute " + num(attr) + " beyond the signature");
        static const char* const kChan[] = {"x", "y", "z", "w"};
        c.vwrite(c.f(Field::VDST), "asuint(AM_IN" + num(attr) + "." + kChan[chan & 3] + ")");
        return true;
      };
    }
    // 16-bit attributes, two halves a dword: SRC0 selects attribute (bits 0-5), channel (6-7)
    // and half (8). The hardware computes P0 + P10 * i (p1ll; p1lv takes P0 from SRC2) and then
    // + P20 * j (p2, into a 16-bit result). Here p1 already gives the whole interpolation, from
    // the three vertices' halves and SV_Barycentrics, and p2 passes its SRC2 on as f16.
    t["v_interp_p1ll_f16"] = t["v_interp_p1lv_f16"] = [](Ctx& c) {
      if (c.input->stage != HwStage::Ps) return c.fail("v_interp_p1 f16 outside a pixel shader");
      const uint32_t sel = c.f(Field::SRC0), attr = sel & 63u, chan = (sel >> 6) & 3u, high = (sel >> 8) & 1u;
      if (attr >= c.input->inputs.size()) return c.fail("v_interp of attribute " + num(attr) + " beyond the signature");
      c.per_vertex_attrs.insert(attr);
      static const char* const kChan[] = {"x", "y", "z", "w"};
      const auto half = [&](int vtx) {
        return "f16tof32((am_vtx" + num(attr) + "[" + std::to_string(vtx) + "]." + kChan[chan] + (high ? " >> 16u" : "") + ") & 0xFFFFu)";
      };
      std::string value;
      if (std::string(c.in->info->name) == "v_interp_p1ll_f16") {
        value = "am_bary.x * " + half(0) + " + am_bary.y * " + half(1) + " + am_bary.z * " + half(2);
      } else {
        value = fsrc16(c, 2) + " + am_bary.y * (" + half(1) + " - " + half(0) + ") + am_bary.z * (" + half(2) + " - " + half(0) + ")";
      }
      c.vwrite(vop_dst(c), "asuint(" + value + ")");
      return true;
    };
    t["v_interp_p2_f16"] = [](Ctx& c) {
      if (c.input->stage != HwStage::Ps) return c.fail("v_interp_p2 f16 outside a pixel shader");
      const uint32_t sel = c.f(Field::SRC0), attr = sel & 63u;
      if (attr >= c.input->inputs.size()) return c.fail("v_interp of attribute " + num(attr) + " beyond the signature");
      c.per_vertex_attrs.insert(attr);
      write16(c, f16_result(c, "asfloat(" + c.src(c.f(Field::SRC2)) + ")"));
      return true;
    };
    // Relative VGPR moves: M0 (s124) indexes the source (movrels), the destination (movreld)
    // or both (movrelsd), kept inside the program's VGPRs.
    t["v_movrels_b32"] = [](Ctx& c) {
      if (!vector_form(c)) return false;
      c.uses_movrel = true;
      const uint32_t sr = c.f(Field::SRC0);
      if (sr < 256) return c.fail("v_movrels_b32 of a scalar source");
      c.touch_v(sr - 256);
      c.vwrite(vop_dst(c), "v[min(" + num(sr - 256) + "u + s[124], am_nv - 1u)]");
      return true;
    };
    t["v_movreld_b32"] = [](Ctx& c) {
      if (!vector_form(c)) return false;
      c.uses_movrel = true;
      const uint32_t d = vop_dst(c);
      c.touch_v(d);
      c.line("if (AM_ACT) { uint x = " + c.src(c.f(Field::SRC0)) + "; v[min(" + num(d) + "u + s[124], am_nv - 1u)] = x; }");
      return true;
    };
    // Swaps two VGPRs (VOP1: the destination and src0, which must be a VGPR).
    t["v_swap_b32"] = [](Ctx& c) {
      if (!vector_form(c)) return false;
      const uint32_t sr = c.f(Field::SRC0), d = vop_dst(c);
      if (sr < 256) return c.fail("v_swap_b32 of a scalar source");
      c.touch_v(sr - 256);
      c.touch_v(d);
      c.line("{");
      c.line("uint am_swap_a = " + c.src(sr) + ", am_swap_b = " + c.src(256 + d) + ";");
      c.vwrite(d, "am_swap_a");
      c.vwrite(sr - 256, "am_swap_b");
      c.line("}");
      return true;
    };
    t["v_movrelsd_b32"] = [](Ctx& c) {
      if (!vector_form(c)) return false;
      c.uses_movrel = true;
      const uint32_t sr = c.f(Field::SRC0), d = vop_dst(c);
      if (sr < 256) return c.fail("v_movrelsd_b32 of a scalar source");
      c.touch_v(sr - 256);
      c.touch_v(d);
      c.line("if (AM_ACT) { uint x = v[min(" + num(sr - 256) + "u + s[124], am_nv - 1u)]; v[min(" + num(d) +
             "u + s[124], am_nv - 1u)] = x; }");
      return true;
    };
    t["s_barrier"] = [](Ctx& c) {
      c.line("GroupMemoryBarrierWithGroupSync();");
      return true;
    };

    // -- scalar memory --
    for (int n : {1, 2, 4, 8, 16}) {
      const std::string sfx = n == 1 ? "" : "x" + std::to_string(n);
      for (const bool buffer : {false, true}) {
        t[(buffer ? "s_buffer_load_dword" : "s_load_dword") + sfx] = [n, buffer](Ctx& c) {
          const uint32_t base = c.f(Field::SBASE) * 2, d = c.f(Field::SDATA);
          // The 21-bit offset is signed; SOFFSET 125 (null) adds nothing.
          const int32_t imm = static_cast<int32_t>(c.f(Field::OFFSET) << 11) >> 11;
          const uint32_t so = c.f(Field::SOFFSET);
          const std::string addr = (buffer ? "am_u64(s[" + num(base) + "], s[" + num(base + 1) + "] & 0xFFFFu)"
                                           : "am_u64(s[" + num(base) + "], s[" + num(base + 1) + "])") +
                                   " + uint64_t(int64_t(" + std::to_string(imm) + "))" +
                                   (so == 125 ? std::string() : " + uint64_t(" + c.src(so) + ")");
          c.line("{ uint64_t p = " + addr + ";");
          for (int k = 0; k < n; ++k) c.line("  " + c.sdst(d + k) + " = am_load(p + " + num(4 * k) + ");");
          c.line("}");
          return true;
        };
      }
    }
    // -- buffer and global memory: raw dwords, and typed (the V#'s format, or the instruction's) --
    for (int n : {1, 2, 3, 4}) {
      const std::string sfx = n == 1 ? "" : "x" + std::to_string(n);
      static const char* const kChannels[] = {"x", "xy", "xyz", "xyzw"};
      for (const bool store : {false, true}) {
        for (const bool tbuffer : {false, true}) {
          const std::string name = std::string(tbuffer ? "tbuffer_" : "buffer_") + (store ? "store" : "load") + "_format_" + kChannels[n - 1];
          t[name] = [n, store, tbuffer](Ctx& c) {
            if (!buffer_address(c)) return false;
            const uint32_t vd = c.f(Field::VDATA);
            c.touch_v(vd + n - 1);
            const std::string fmt = tbuffer ? num(c.f(Field::FORMAT)) + "u" : "((rs.w >> 12) & 0x7Fu)";
            if (store) {
              std::string val = "uint4(";
              for (int k = 0; k < 4; ++k) val += (k ? ", " : "") + (k < n ? "v[" + num(vd + k) + "]" : std::string("0u"));
              c.line("  am_fmt_store(p, lim, am_fmt_info(" + fmt + "), " + num(n) + "u, " + val + "));");
            } else {
              c.line("  uint4 r = am_fmt_load(p, lim, am_fmt_info(" + fmt + "), rs.w);");
              for (int k = 0; k < n; ++k) c.line("  v[" + num(vd + k) + "] = r[" + num(k) + "];");
            }
            c.line("}");
            return true;
          };
        }
        for (const bool tbuffer : {false, true}) {
          const std::string name = std::string(tbuffer ? "tbuffer_" : "buffer_") + (store ? "store" : "load") + "_format_d16_" + kChannels[n - 1];
          t[name] = [n, store, tbuffer](Ctx& c) { return buffer_d16(c, n, store, tbuffer, false); };
        }
        t[(store ? "buffer_store_dword" : "buffer_load_dword") + sfx] = [n, store](Ctx& c) {
          if (!buffer_address(c)) return false;
          const uint32_t vd = c.f(Field::VDATA);
          c.touch_v(vd + n - 1);
          for (int k = 0; k < n; ++k) {
            if (store) c.line("  if (" + num(4 * k) + "u < lim) am_store(p + " + num(4 * k) + ", v[" + num(vd + k) + "]);");
            else c.line("  v[" + num(vd + k) + "] = " + num(4 * k) + "u < lim ? am_load(p + " + num(4 * k) + ") : 0u;");
          }
          c.line("}");
          return true;
        };
        t[(store ? "global_store_dword" : "global_load_dword") + sfx] = [n, store](Ctx& c) {
          const uint32_t sa = c.f(Field::SADDR), va = c.f(Field::ADDR);
          const uint32_t vd = store ? c.f(Field::DATA) : c.f(Field::VDST);
          const int32_t imm = static_cast<int32_t>(c.f(Field::OFFSET) << 20) >> 20;
          std::string addr;
          if (sa == 125 || sa == 0x7F) {
            c.touch_v(va + 1);
            addr = "am_u64(v[" + num(va) + "], v[" + num(va + 1) + "])";
          } else {
            c.touch_v(va);
            addr = "am_u64(s[" + num(sa) + "], s[" + num(sa + 1) + "]) + uint64_t(v[" + num(va) + "])";
          }
          c.touch_v(vd + n - 1);
          c.line("if (AM_ACT) { uint64_t p = " + addr + " + uint64_t(int64_t(" + std::to_string(imm) + "));");
          for (int k = 0; k < n; ++k) {
            if (store) c.line("  am_store(p + " + num(4 * k) + ", v[" + num(vd + k) + "]);");
            else c.line("  v[" + num(vd + k) + "] = am_load(p + " + num(4 * k) + ");");
          }
          c.line("}");
          return true;
        };
      }
    }
    // Sub-dword buffer loads: the dword holding the bytes, shifted down, masked or sign-extended.
    {
      struct Narrow {
        const char* name;
        const char* expr;  // of x, the bytes in the low bits
      };
      static const Narrow kNarrow[] = {{"buffer_load_ubyte", "(x & 0xFFu)"},
                                       {"buffer_load_sbyte", "uint(int(x << 24) >> 24)"},
                                       {"buffer_load_ushort", "(x & 0xFFFFu)"},
                                       {"buffer_load_sshort", "uint(int(x << 16) >> 16)"}};
      for (const Narrow& n : kNarrow) {
        const std::string expr = n.expr;
        t[n.name] = [expr](Ctx& c) {
          if (!buffer_address(c)) return false;
          const uint32_t vd = c.f(Field::VDATA);
          c.touch_v(vd);
          c.line("  { uint x = 0u < lim ? am_load(p) >> (uint(p & 3u) * 8u) : 0u; v[" + num(vd) + "] = " + expr + "; }");
          c.line("}");
          return true;
        };
      }
    }
    // -- atomics: 32-bit; GLC returns the value before. The ops' numbers are am_atomic's: 0-10 the
    // integer ones below, 11 and 12 the float min and max (global only so far). --
    {
      static const char* const kAtomics[] = {"swap", "cmpswap", "add", "sub", "smin", "umin", "smax", "umax", "and", "or", "xor"};
      const auto global_atomic = [](uint32_t op) {
        return [op](Ctx& c) {
          const uint32_t sa = c.f(Field::SADDR), va = c.f(Field::ADDR), vd = c.f(Field::DATA), dst = c.f(Field::VDST);
          const int32_t imm = static_cast<int32_t>(c.f(Field::OFFSET) << 20) >> 20;
          std::string addr;
          if (sa == 125 || sa == 0x7F) {
            c.touch_v(va + 1);
            addr = "am_u64(v[" + num(va) + "], v[" + num(va + 1) + "])";
          } else {
            c.touch_v(va);
            addr = "am_u64(s[" + num(sa) + "], s[" + num(sa + 1) + "]) + uint64_t(v[" + num(va) + "])";
          }
          c.touch_v(vd + (op == 1 ? 1 : 0));
          const std::string cmp = op == 1 ? "v[" + num(vd + 1) + "]" : "0u";
          c.line("if (AM_ACT) { uint o = am_atomic(" + num(op) + "u, " + addr + " + uint64_t(int64_t(" + std::to_string(imm) +
                 ")), v[" + num(vd) + "], " + cmp + ");");
          if (c.f(Field::GLC)) c.line("  " + c.vdst(dst) + " = o;");
          c.line("}");
          return true;
        };
      };
      for (uint32_t op = 0; op < 11; ++op) {
        t[std::string("buffer_atomic_") + kAtomics[op]] = [op](Ctx& c) {
          if (!buffer_address(c)) return false;
          const uint32_t vd = c.f(Field::VDATA);
          c.touch_v(vd + (op == 1 ? 1 : 0));
          const std::string cmp = op == 1 ? "v[" + num(vd + 1) + "]" : "0u";
          c.line("  uint o = 4u <= lim ? am_atomic(" + num(op) + "u, p, v[" + num(vd) + "], " + cmp + ") : 0u;");
          if (c.f(Field::GLC)) c.line("  v[" + num(vd) + "] = o;");
          c.line("}");
          return true;
        };
        t[std::string("global_atomic_") + kAtomics[op]] = global_atomic(op);
      }
      t["global_atomic_fmin"] = global_atomic(11);
      t["global_atomic_fmax"] = global_atomic(12);
    }
    // -- images --
    {
      const auto add = [&t](const char* name, ImgOp op) { t[name] = [op](Ctx& c) { return image(c, op); }; };
      ImgOp o{ImgKind::Sample};
      o.implicit = true;
      add("image_sample", o);
      o.bias = true;
      add("image_sample_b", o);
      add("image_sample_lz", ImgOp{ImgKind::Sample});
      o = ImgOp{ImgKind::Sample};
      o.lod = true;
      add("image_sample_l", o);
      o = ImgOp{ImgKind::Sample};
      o.grads = true;
      add("image_sample_d", o);
      o = ImgOp{ImgKind::Sample};
      o.compare = true;
      add("image_sample_c_lz", o);
      // Gather reads the top level, as D3D's does: gather4 (implicit lod) and gather4_lz.
      add("image_gather4", ImgOp{ImgKind::Gather});
      add("image_gather4_lz", ImgOp{ImgKind::Gather});
      o = ImgOp{ImgKind::Gather};
      o.offset = true;
      add("image_gather4_o", o);
      add("image_gather4_lz_o", o);
      add("image_load", ImgOp{ImgKind::Load});
      o = ImgOp{ImgKind::Load};
      o.lod = true;
      add("image_load_mip", o);
      add("image_store", ImgOp{ImgKind::Store});
      add("image_get_resinfo", ImgOp{ImgKind::Info});
      add("image_get_lod", ImgOp{ImgKind::Lod});
      static const char* const kAtomics[] = {"swap", "cmpswap", "add", "sub", "smin", "umin", "smax", "umax", "and", "or", "xor"};
      for (uint32_t k = 0; k < 11; ++k) {
        o = ImgOp{ImgKind::Atomic};
        o.atomic = k;
        t[std::string("image_atomic_") + kAtomics[k]] = [o](Ctx& c) { return image(c, o); };
      }
    }
    t["buffer_load_format_d16_hi_x"] = [](Ctx& c) { return buffer_d16(c, 1, false, false, true); };
    t["buffer_store_format_d16_hi_x"] = [](Ctx& c) { return buffer_d16(c, 1, true, false, true); };
    // -- LDS --
    t["ds_read_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      c.touch_v(c.f(Field::ADDR));
      c.vwrite(c.f(Field::VDST), "am_lds[(v[" + num(c.f(Field::ADDR)) + "] + " + num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) + "u) >> 2]");
      return true;
    };
    t["ds_read_b64"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR), d = c.f(Field::VDST);
      c.touch_v(a);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint b = (v[" + num(a) + "] + " + num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) +
             "u) >> 2; v[" + num(d) + "] = am_lds[b]; v[" + num(d + 1) + "] = am_lds[b + 1u]; }");
      return true;
    };
    t["ds_write_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      c.touch_v(c.f(Field::ADDR));
      c.touch_v(c.f(Field::DATA0));
      c.line("if (AM_ACT) am_lds[(v[" + num(c.f(Field::ADDR)) + "] + " + num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) +
             "u) >> 2] = v[" + num(c.f(Field::DATA0)) + "];");
      return true;
    };
    // 16 bits into their half of an LDS dword (atomically: lanes may write both halves of one).
    t["ds_write_b16"] = [](Ctx& c) {
      c.uses_lds = true;
      c.touch_v(c.f(Field::ADDR));
      c.touch_v(c.f(Field::DATA0));
      c.line("if (AM_ACT) { uint a = v[" + num(c.f(Field::ADDR)) + "] + " + num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) +
             "u; uint sh = (a & 2u) * 8u; uint o; InterlockedAnd(am_lds[a >> 2], ~(0xFFFFu << sh), o); "
             "InterlockedOr(am_lds[a >> 2], (v[" + num(c.f(Field::DATA0)) + "] & 0xFFFFu) << sh, o); }");
      return true;
    };
    // Compare and store: the dword becomes DATA1 where it equals DATA0; the value before returns.
    t["ds_cmpst_rtn_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR), d0 = c.f(Field::DATA0), d1 = c.f(Field::DATA1);
      c.touch_v(a);
      c.touch_v(d0);
      c.touch_v(d1);
      c.line("if (AM_ACT) { uint o; InterlockedCompareExchange(am_lds[(v[" + num(a) + "] + " +
             num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) + "u) >> 2], v[" + num(d0) + "], v[" + num(d1) + "], o); " +
             c.vdst(c.f(Field::VDST)) + " = o; }");
      return true;
    };
    // Backward permute: each lane reads DATA0 from the lane its address (in bytes) names.
    t["ds_bpermute_b32"] = [](Ctx& c) {
      const uint32_t a = c.f(Field::ADDR), d0 = c.f(Field::DATA0), off = c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8;
      c.touch_v(a);
      c.touch_v(d0);
      c.line("{ uint l = ((v[" + num(a) + "] + " + num(off) + "u) >> 2) & (WaveGetLaneCount() - 1u); uint x = WaveReadLaneAt(v[" +
             num(d0) + "], l);");
      c.line("  if (AM_ACT) " + c.vdst(c.f(Field::VDST)) + " = x; }");
      return true;
    };
    t["ds_write_b64"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR), d = c.f(Field::DATA0);
      c.touch_v(a);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint b = (v[" + num(a) + "] + " + num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) +
             "u) >> 2; am_lds[b] = v[" + num(d) + "]; am_lds[b + 1u] = v[" + num(d + 1) + "]; }");
      return true;
    };
    t["ds_read2_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR), d = c.f(Field::VDST);
      c.touch_v(a);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint b = v[" + num(a) + "] >> 2; v[" + num(d) + "] = am_lds[b + " + num(c.f(Field::OFFSET0)) +
             "u]; v[" + num(d + 1) + "] = am_lds[b + " + num(c.f(Field::OFFSET1)) + "u]; }");
      return true;
    };
    // The st64 pairs: offsets in units of 64 dwords.
    t["ds_read2st64_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR), d = c.f(Field::VDST);
      c.touch_v(a);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint b = v[" + num(a) + "] >> 2; v[" + num(d) + "] = am_lds[b + " + num(64 * c.f(Field::OFFSET0)) +
             "u]; v[" + num(d + 1) + "] = am_lds[b + " + num(64 * c.f(Field::OFFSET1)) + "u]; }");
      return true;
    };
    t["ds_write2st64_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR);
      c.touch_v(a);
      c.touch_v(c.f(Field::DATA0));
      c.touch_v(c.f(Field::DATA1));
      c.line("if (AM_ACT) { uint b = v[" + num(a) + "] >> 2; am_lds[b + " + num(64 * c.f(Field::OFFSET0)) + "u] = v[" +
             num(c.f(Field::DATA0)) + "]; am_lds[b + " + num(64 * c.f(Field::OFFSET1)) + "u] = v[" + num(c.f(Field::DATA1)) + "]; }");
      return true;
    };
    // LDS atomics (32-bit, unsigned forms); the _rtn forms return the value before.
    for (const auto& [op, fn] : {std::pair{"add_u32", "InterlockedAdd"}, {"sub_u32", "InterlockedAdd"}, {"min_u32", "InterlockedMin"},
                                 {"max_u32", "InterlockedMax"}, {"and_b32", "InterlockedAnd"}, {"or_b32", "InterlockedOr"},
                                 {"xor_b32", "InterlockedXor"}, {"wrxchg_rtn_b32", "InterlockedExchange"}}) {
      const std::string o = op;
      const bool sub = o == "sub_u32";
      for (const bool rtn : {false, true}) {
        if (o == "wrxchg_rtn_b32" && !rtn) continue;
        const std::string name = o == "wrxchg_rtn_b32" ? "ds_wrxchg_rtn_b32" : "ds_" + (rtn ? o.substr(0, o.find('_')) + "_rtn" + o.substr(o.find('_')) : o);
        t[name] = [fn = std::string(fn), sub, rtn](Ctx& c) {
          c.uses_lds = true;
          const uint32_t a = c.f(Field::ADDR), d0 = c.f(Field::DATA0);
          c.touch_v(a);
          c.touch_v(d0);
          const std::string x = sub ? "0u - v[" + num(d0) + "]" : "v[" + num(d0) + "]";
          std::string l = "if (AM_ACT) { uint o; " + fn + "(am_lds[(v[" + num(a) + "] + " +
                          num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) + "u) >> 2], " + x + ", o);";
          if (rtn) l += " " + c.vdst(c.f(Field::VDST)) + " = o;";
          c.line(l + " }");
          return true;
        };
      }
    }
    // ds_swizzle_b32: offset bit 15 picks quad mode (four 2-bit lane selects), else bit mode in
    // groups of 32 lanes: ((lane & and) | or) ^ xor. A disabled source lane reads 0.
    t["ds_swizzle_b32"] = [](Ctx& c) {
      const uint32_t a = c.f(Field::ADDR), d = c.f(Field::VDST), off = c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8;
      c.touch_v(a);
      std::string lane;
      if (off & 0x8000) {
        lane = "((am_lane & ~3u) | ((" + num(off & 0xFF) + "u >> ((am_lane & 3u) * 2u)) & 3u))";
      } else {
        const uint32_t and_m = off & 31, or_m = (off >> 5) & 31, xor_m = (off >> 10) & 31;
        lane = "((am_lane & ~31u) | ((((am_lane & 31u) & " + num(and_m) + "u) | " + num(or_m) + "u) ^ " + num(xor_m) + "u))";
      }
      c.line("{ uint l = " + lane + "; uint x = WaveReadLaneAt(v[" + num(a) + "], l);");
      c.line("  if (AM_ACT) " + c.vdst(d) + " = am_lane_on(l) ? x : 0u; }");
      return true;
    };
    t["ds_write2_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR);
      c.touch_v(a);
      c.touch_v(c.f(Field::DATA0));
      c.touch_v(c.f(Field::DATA1));
      c.line("if (AM_ACT) { uint b = v[" + num(a) + "] >> 2; am_lds[b + " + num(c.f(Field::OFFSET0)) + "u] = v[" +
             num(c.f(Field::DATA0)) + "]; am_lds[b + " + num(c.f(Field::OFFSET1)) + "u] = v[" + num(c.f(Field::DATA1)) + "]; }");
      return true;
    };
    return t;
  }();
  return handlers;
}

// The helpers every program gets, and the binding model's memory functions.
const char* kHelpers = R"(
uint64_t am_u64(uint lo, uint hi) { return (uint64_t(hi) << 32) | lo; }
// f32 to f16, rounded to nearest even as the console does: f32tof16 truncates on some GPUs
// (seen on RDNA 4), so the next half up is taken when it is nearer (ties to the even one).
uint am_f16(float f) {
  uint h = f32tof16(f);
  if ((h & 0x7C00u) == 0x7C00u || f != f) return h;
  uint m = h & 0x7FFFu;
  float a = abs(f), d0 = abs(f16tof32(m) - a);
  if (m < 0x7BFFu) {
    float d1 = abs(f16tof32(m + 1u) - a);
    if (d1 < d0 || (d1 == d0 && ((m + 1u) & 1u) == 0u)) m += 1u;
  }
  return (h & 0x8000u) | m;
}
uint am_bfe_u32(uint a, uint off, uint width) {
  off &= 31u; width &= 31u;
  return width == 0u ? 0u : (a >> off) & ((1u << width) - 1u);
}
uint am_bfe_i32(uint a, uint off, uint width) {
  off &= 31u; width &= 31u;
  if (width == 0u) return 0u;
  return asuint(asint(a << (32u - off - width)) >> (32u - width));
}
uint am_cvt_u32(float f) { return f != f || f <= 0.0f ? 0u : f >= 4294967295.0f ? 0xFFFFFFFFu : uint(f); }
int am_cvt_i32(float f) { return f != f ? 0 : f <= -2147483648.0f ? int(0x80000000u) : f >= 2147483647.0f ? 0x7FFFFFFF : int(f); }
// SDWA's destination: the result's low bits in a byte or word; the other bits zero, the sign,
// or what the register held (DST_UNUSED 0, 1, 2).
uint am_sdwa_dst(uint old, uint r, uint sh, uint w, uint mode) {
  uint m = ((1u << w) - 1u) << sh;
  uint x = (r << sh) & m;
  if (mode == 2u) return (old & ~m) | x;
  if (mode == 1u && ((r >> (w - 1u)) & 1u) != 0u && sh + w < 32u) x |= 0xFFFFFFFFu << (sh + w);
  return x;
}
// v_cube*: (face id, s, t, 2 * major axis) for the direction (x, y, z).
float4 am_cube(float x, float y, float z) {
  if (abs(z) >= abs(x) && abs(z) >= abs(y)) return float4(z < 0.0f ? 5.0f : 4.0f, z < 0.0f ? -x : x, -y, 2.0f * z);
  if (abs(y) >= abs(x)) return float4(y < 0.0f ? 3.0f : 2.0f, x, y < 0.0f ? -z : z, 2.0f * y);
  return float4(x < 0.0f ? 1.0f : 0.0f, x < 0.0f ? z : -z, -y, 2.0f * x);
}
uint am_wqm(uint a) {
  uint x = a | (a >> 1);
  x |= x >> 2;  // bit 4q: any bit of nibble q
  return (x & 0x11111111u) * 0xFu;
}
// v_perm_b32: each selector byte picks a byte of {s0, s1} or a constant.
uint am_perm_byte(uint64_t x, uint k) {
  if (k <= 7u) return uint(x >> (8u * k)) & 0xFFu;
  if (k <= 11u) return (uint(x >> (8u * (2u * (k - 8u) + 1u))) & 0x80u) ? 0xFFu : 0u;  // a sign, spread
  return k == 12u ? 0u : 0xFFu;
}
uint am_perm(uint s0, uint s1, uint sel) {
  uint64_t x = (uint64_t(s0) << 32) | s1;
  return am_perm_byte(x, sel & 0xFFu) | (am_perm_byte(x, (sel >> 8) & 0xFFu) << 8) |
         (am_perm_byte(x, (sel >> 16) & 0xFFu) << 16) | (am_perm_byte(x, sel >> 24) << 24);
}
uint am_cvt_pk_u8(float f, uint byte_index, uint into) {
  uint v = am_cvt_u32(round(f));
  v = min(v, 255u);
  uint sh = 8u * (byte_index & 3u);
  return (into & ~(0xFFu << sh)) | (v << sh);
}
// v_cmp_class_f32: the mask's bits name classes.
bool am_class(float f, uint mask) {
  uint u = asuint(f);
  bool neg = (u >> 31) != 0u;
  uint e = (u >> 23) & 0xFFu, m = u & 0x7FFFFFu;
  uint c;
  if (e == 0xFFu) c = m == 0u ? (neg ? 2u : 9u) : ((m & 0x400000u) ? 1u : 0u);
  else if (e == 0u) c = m == 0u ? (neg ? 5u : 6u) : (neg ? 4u : 7u);
  else c = neg ? 3u : 8u;
  return ((mask >> c) & 1u) != 0u;
}
)";

const char* kTestBinding = R"(
// The tests' binding model: GPU addresses map onto one buffer from am_args.base.
RWByteAddressBuffer am_memory : register(u0);
RWByteAddressBuffer am_dump : register(u1);
RWByteAddressBuffer am_bindings : register(u2);  // image_bindings.h
cbuffer am_args : register(b0) { uint4 am_user[8]; uint2 am_base; uint2 am_pad; };
uint am_offset(uint64_t a) { return uint(a - am_u64(am_base.x, am_base.y)); }
uint am_load(uint64_t a) { return am_memory.Load(am_offset(a)); }
void am_store(uint64_t a, uint x) { am_memory.Store(am_offset(a), x); }
// 32-bit atomics by number (swap, cmpswap, add, sub, smin, umin, smax, umax, and, or, xor);
// the value before. cmpswap writes x where the memory equals `cmp`.
uint am_atomic(uint op, uint64_t a, uint x, uint cmp) {
  uint at = am_offset(a), o = 0u;
  if (op == 0u) am_memory.InterlockedExchange(at, x, o);
  else if (op == 1u) am_memory.InterlockedCompareExchange(at, cmp, x, o);
  else if (op == 2u) am_memory.InterlockedAdd(at, x, o);
  else if (op == 3u) am_memory.InterlockedAdd(at, 0u - x, o);
  else if (op == 4u) { int so; am_memory.InterlockedMin(at, asint(x), so); o = asuint(so); }
  else if (op == 5u) am_memory.InterlockedMin(at, x, o);
  else if (op == 6u) { int so; am_memory.InterlockedMax(at, asint(x), so); o = asuint(so); }
  else if (op == 7u) am_memory.InterlockedMax(at, x, o);
  else if (op == 8u) am_memory.InterlockedAnd(at, x, o);
  else if (op == 9u) am_memory.InterlockedOr(at, x, o);
  else if (op == 10u) am_memory.InterlockedXor(at, x, o);
  else {  // 11 fmin, 12 fmax: a compare-and-swap loop (float atomics have no HLSL form)
    o = am_memory.Load(at);
    for (uint k = 0u; k < 64u; ++k) {
      float r = op == 11u ? min(asfloat(o), asfloat(x)) : max(asfloat(o), asfloat(x));
      uint prev;
      am_memory.InterlockedCompareExchange(at, o, asuint(r), prev);
      if (prev == o) break;
      o = prev;
    }
  }
  return o;
}
// Writes the bits of `m` only; other lanes may write the rest of the dword at the same time.
void am_store_masked(uint64_t a, uint x, uint m) {
  if (m == 0xFFFFFFFFu) { am_store(a, x); return; }
  am_memory.InterlockedAnd(am_offset(a), ~m);
  am_memory.InterlockedOr(am_offset(a), x & m);
}
)";

// The runtime's binding model (runtime/d3d12x/gpu_memory.h): the user data SGPRs are root
// constants; console GPU addresses go through the page table (shader heap view 0) to the
// window of the title allocation holding them. Addresses with no window read 0 and drop
// stores.
const char* kRuntimeBinding = R"(
cbuffer am_args : register(b0) { uint4 am_user[5]; };
RWByteAddressBuffer am_bindings : register(u0);  // image_bindings.h, kept by the runtime
uint am_entry(uint64_t a) {
  if ((a >> 40) != 0) return 0u;
  ByteAddressBuffer t = ResourceDescriptorHeap[0];
  return t.Load(uint(a >> 16) * 4u);
}
uint am_at(uint e, uint64_t a) { return ((e & 0xFFFFu) << 16) | (uint(a) & 0xFFFCu); }
uint am_load(uint64_t a) {
  uint e = am_entry(a);
  if (e == 0u) return 0u;
  RWByteAddressBuffer w = ResourceDescriptorHeap[NonUniformResourceIndex(e >> 16)];
  return w.Load(am_at(e, a));
}
void am_store(uint64_t a, uint x) {
  uint e = am_entry(a);
  if (e == 0u) return;
  RWByteAddressBuffer w = ResourceDescriptorHeap[NonUniformResourceIndex(e >> 16)];
  w.Store(am_at(e, a), x);
}
uint am_atomic(uint op, uint64_t a, uint x, uint cmp) {
  uint e = am_entry(a), o = 0u;
  if (e == 0u) return 0u;
  RWByteAddressBuffer w = ResourceDescriptorHeap[NonUniformResourceIndex(e >> 16)];
  uint at = am_at(e, a);
  if (op == 0u) w.InterlockedExchange(at, x, o);
  else if (op == 1u) w.InterlockedCompareExchange(at, cmp, x, o);
  else if (op == 2u) w.InterlockedAdd(at, x, o);
  else if (op == 3u) w.InterlockedAdd(at, 0u - x, o);
  else if (op == 4u) { int so; w.InterlockedMin(at, asint(x), so); o = asuint(so); }
  else if (op == 5u) w.InterlockedMin(at, x, o);
  else if (op == 6u) { int so; w.InterlockedMax(at, asint(x), so); o = asuint(so); }
  else if (op == 7u) w.InterlockedMax(at, x, o);
  else if (op == 8u) w.InterlockedAnd(at, x, o);
  else if (op == 9u) w.InterlockedOr(at, x, o);
  else if (op == 10u) w.InterlockedXor(at, x, o);
  else {  // 11 fmin, 12 fmax: a compare-and-swap loop (float atomics have no HLSL form)
    o = w.Load(at);
    for (uint k = 0u; k < 64u; ++k) {
      float r = op == 11u ? min(asfloat(o), asfloat(x)) : max(asfloat(o), asfloat(x));
      uint prev;
      w.InterlockedCompareExchange(at, o, asuint(r), prev);
      if (prev == o) break;
      o = prev;
    }
  }
  return o;
}
void am_store_masked(uint64_t a, uint x, uint m) {
  if (m == 0xFFFFFFFFu) { am_store(a, x); return; }
  uint e = am_entry(a);
  if (e == 0u) return;
  RWByteAddressBuffer w = ResourceDescriptorHeap[NonUniformResourceIndex(e >> 16)];
  w.InterlockedAnd(am_at(e, a), ~m);
  w.InterlockedOr(am_at(e, a), x & m);
}
)";

const char* kImageHelpers = R"(
// ---- images: descriptors from the bindings table, views from the descriptor heaps ----
struct AmImage { uint srv; uint alt; uint uav; uint flags; };
struct AmSampler { uint index; uint flags; };
#define AM_VALID 0x80000000u
#define AM_ARR(im) (((im).flags & 4u) != 0u)
#define AM_DX(x) (WaveReadLaneAt(x, am_lane | 1u) - WaveReadLaneAt(x, am_lane & ~1u))
#define AM_DY(x) (WaveReadLaneAt(x, am_lane | 2u) - WaveReadLaneAt(x, am_lane & ~2u))
uint am_bind_hash(uint4 a, uint4 b) {
  uint h = 0x811C9DC5u;
  h = (h ^ a.x) * 0x01000193u; h = (h ^ a.y) * 0x01000193u; h = (h ^ a.z) * 0x01000193u; h = (h ^ a.w) * 0x01000193u;
  h = (h ^ b.x) * 0x01000193u; h = (h ^ b.y) * 0x01000193u; h = (h ^ b.z) * 0x01000193u; h = (h ^ b.w) * 0x01000193u;
  return h ^ (h >> 16);
}
uint am_bind_hash2(uint h) {
  h ^= 0x5BD1E995u;
  h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u;
  return h ^ (h >> 16);
}
uint4 am_bind_slot(uint at, uint4 a, uint4 b) {
  uint4 v = am_bindings.Load4(at + 32u);
  return (v.w != 0u && all(am_bindings.Load4(at) == a) && all(am_bindings.Load4(at + 16u) == b)) ? v : uint4(0u, 0u, 0u, 0u);
}
uint4 am_bind_find(uint table, uint cap, uint size, uint4 a, uint4 b) {
  if (cap == 0u) return uint4(0u, 0u, 0u, 0u);
  uint h = am_bind_hash(a, b);
  uint4 v = am_bind_slot(table + (h & (cap - 1u)) * size, a, b);
  if (v.w == 0u) v = am_bind_slot(table + (am_bind_hash2(h) & (cap - 1u)) * size, a, b);
  return v;
}
// A descriptor the table lacks: queued once a wave for the host.
void am_bind_miss(uint kind, uint4 a, uint4 b) {
  if (!WaveIsFirstLane()) return;
  uint n;
  am_bindings.InterlockedAdd(24u, 1u, n);
  if (n >= am_bindings.Load(20u)) return;
  uint at = am_bindings.Load(16u) + n * 48u;
  am_bindings.Store4(at, uint4(kind, 0u, 0u, 0u));
  am_bindings.Store4(at + 16u, a);
  am_bindings.Store4(at + 32u, b);
}
AmImage am_image(uint4 a, uint4 b) {
  uint4 v = am_bind_find(am_bindings.Load(0u), am_bindings.Load(4u), 64u, a, b);
  if (v.w == 0u) am_bind_miss(1u, a, b);
  AmImage im;
  im.srv = WaveReadLaneFirst(v.x); im.alt = WaveReadLaneFirst(v.y); im.uav = WaveReadLaneFirst(v.z); im.flags = WaveReadLaneFirst(v.w);
  return im;
}
AmSampler am_sampler(uint4 a) {
  uint4 v = am_bind_find(am_bindings.Load(8u), am_bindings.Load(12u), 48u, a, uint4(0u, 0u, 0u, 0u));
  if (v.w == 0u) am_bind_miss(2u, a, uint4(0u, 0u, 0u, 0u));
  AmSampler sm;
  sm.index = WaveReadLaneFirst(v.x); sm.flags = WaveReadLaneFirst(v.w);
  return sm;
}

// A view of the image's numeric class (the format decides: float, uint or sint).
#define AM_CLASS_DO(T, idx, st) { uint cl_ = im.flags & 3u; \
  if (cl_ == 0u) { T<float4> t = ResourceDescriptorHeap[idx]; st; } \
  else if (cl_ == 1u) { T<uint4> t = ResourceDescriptorHeap[idx]; st; } \
  else { T<int4> t = ResourceDescriptorHeap[idx]; st; } }
#define AM_CLASS_RET(T, idx, e) AM_CLASS_DO(T, idx, return asuint(e))
#define AM_CLASS_STORE(T, idx, at) { uint cl_ = im.flags & 3u; \
  if (cl_ == 0u) { T<float4> t = ResourceDescriptorHeap[idx]; t[at] = asfloat(d); } \
  else if (cl_ == 1u) { T<uint4> t = ResourceDescriptorHeap[idx]; t[at] = d; } \
  else { T<int4> t = ResourceDescriptorHeap[idx]; t[at] = asint(d); } return; }
#define AM_NONE uint4(0u, 0u, 0u, 0u)

// Loads: c = (x, y, z or slice, sample); out of bounds reads zero.
uint4 am_load_1d(AmImage im, int4 c, int mip) {
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if ((im.flags & 16u) != 0u) {
    if (AM_ARR(im)) AM_CLASS_RET(RWTexture1DArray, im.uav, t[uint2(c.xy)])
    AM_CLASS_RET(RWTexture1D, im.uav, t[uint(c.x)])
  }
  if (AM_ARR(im)) AM_CLASS_RET(Texture1DArray, im.srv, t.Load(int3(c.xy, mip)))
  AM_CLASS_RET(Texture1D, im.srv, t.Load(int2(c.x, mip)))
}
uint4 am_load_2d(AmImage im, int4 c, int mip) {
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if ((im.flags & 16u) != 0u) {
    if (AM_ARR(im)) AM_CLASS_RET(RWTexture2DArray, im.uav, t[uint3(c.xyz)])
    AM_CLASS_RET(RWTexture2D, im.uav, t[uint2(c.xy)])
  }
  if (AM_ARR(im)) AM_CLASS_RET(Texture2DArray, im.srv, t.Load(int4(c.xyz, mip)))
  AM_CLASS_RET(Texture2D, im.srv, t.Load(int3(c.xy, mip)))
}
uint4 am_load_3d(AmImage im, int4 c, int mip) {
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if ((im.flags & 16u) != 0u) AM_CLASS_RET(RWTexture3D, im.uav, t[uint3(c.xyz)])
  AM_CLASS_RET(Texture3D, im.srv, t.Load(int4(c.xyz, mip)))
}
// A cube's faces are loaded through its 2D-array views (z = face, or layer of a cube array).
uint4 am_load_cube(AmImage im, int4 c, int mip) {
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if ((im.flags & 16u) != 0u) AM_CLASS_RET(RWTexture2DArray, im.uav, t[uint3(c.xyz)])
  AM_CLASS_RET(Texture2DArray, im.alt, t.Load(int4(c.xyz, mip)))
}
uint4 am_load_ms(AmImage im, int4 c, int mip) {
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if (AM_ARR(im)) AM_CLASS_RET(Texture2DMSArray, im.srv, t.Load(int3(c.xyz), c.w))
  AM_CLASS_RET(Texture2DMS, im.srv, t.Load(int2(c.xy), c.w))
}

// Stores, through the uav.
void am_store_1d(AmImage im, int4 c, uint4 d) {
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return;
  if (AM_ARR(im)) AM_CLASS_STORE(RWTexture1DArray, im.uav, uint2(c.xy))
  AM_CLASS_STORE(RWTexture1D, im.uav, uint(c.x))
}
void am_store_2d(AmImage im, int4 c, uint4 d) {
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return;
  if (AM_ARR(im)) AM_CLASS_STORE(RWTexture2DArray, im.uav, uint3(c.xyz))
  AM_CLASS_STORE(RWTexture2D, im.uav, uint2(c.xy))
}
void am_store_3d(AmImage im, int4 c, uint4 d) {
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return;
  AM_CLASS_STORE(RWTexture3D, im.uav, uint3(c.xyz))
}
void am_store_cube(AmImage im, int4 c, uint4 d) {
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return;
  AM_CLASS_STORE(RWTexture2DArray, im.uav, uint3(c.xyz))
}

// The console's cube coordinates (s, t in [1, 2] on the face, face + 8 * layer) as a direction
// and layer: the inverse of v_cubesc / v_cubetc / v_cubeid with s = sc / |ma| + 1.5.
float4 am_cube_dir(float3 c) {
  float u = (c.x - 1.5f) * 2.0f, v = (c.y - 1.5f) * 2.0f;
  float layer = floor((c.z + 0.5f) / 8.0f);
  uint f = uint(round(c.z - 8.0f * layer));
  float3 d;
  if (f == 0u) d = float3(1.0f, -v, -u);
  else if (f == 1u) d = float3(-1.0f, -v, u);
  else if (f == 2u) d = float3(u, 1.0f, v);
  else if (f == 3u) d = float3(u, -1.0f, -v);
  else if (f == 4u) d = float3(u, -v, 1.0f);
  else d = float3(-u, -v, -1.0f);
  return float4(d, layer);
}

#define AM_SAMPLED(im, sm) (((im).flags & (sm).flags & AM_VALID) != 0u)
// Sampling at a level: c = (x, slice) / (x, y, slice) / (x, y, z) / (direction, layer).
float4 am_sample_1d(AmImage im, AmSampler sm, float2 c, float lod) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { Texture1DArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleLevel(s, c, lod); }
  Texture1D<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleLevel(s, c.x, lod);
}
float4 am_sample_2d(AmImage im, AmSampler sm, float3 c, float lod) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { Texture2DArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleLevel(s, c, lod); }
  Texture2D<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleLevel(s, c.xy, lod);
}
float4 am_sample_3d(AmImage im, AmSampler sm, float3 c, float lod) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  Texture3D<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleLevel(s, c, lod);
}
float4 am_sample_cube(AmImage im, AmSampler sm, float4 c, float lod) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { TextureCubeArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleLevel(s, c, lod); }
  TextureCube<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleLevel(s, c.xyz, lod);
}
)";
// Level-of-detail helpers, only for programs with image_get_lod (the others keep their source).
const char* kLodHelpers = R"(
// The level of detail for these gradients: (clamped to the image's levels, unclamped). The
// sampler's own lod clamps are not applied.
float2 am_lod_of(float r, uint mips) {
  float l = 0.5f * log2(max(r, 1e-30f));
  return float2(clamp(l, 0.0f, float(max(mips, 1u) - 1u)), l);
}
float2 am_lod_1d(AmImage im, AmSampler sm, float dx, float dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  uint w, e, m;
  if (AM_ARR(im)) { Texture1DArray<float4> t = ResourceDescriptorHeap[im.srv]; t.GetDimensions(0, w, e, m); }
  else { Texture1D<float4> t1 = ResourceDescriptorHeap[im.srv]; t1.GetDimensions(0, w, m); }
  float a = dx * w, b = dy * w;
  return am_lod_of(max(a * a, b * b), m);
}
float2 am_lod_2d(AmImage im, AmSampler sm, float2 dx, float2 dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  uint w, h, e, m;
  if (AM_ARR(im)) { Texture2DArray<float4> t = ResourceDescriptorHeap[im.srv]; t.GetDimensions(0, w, h, e, m); }
  else { Texture2D<float4> t1 = ResourceDescriptorHeap[im.srv]; t1.GetDimensions(0, w, h, m); }
  float2 a = dx * float2(w, h), b = dy * float2(w, h);
  return am_lod_of(max(dot(a, a), dot(b, b)), m);
}
float2 am_lod_3d(AmImage im, AmSampler sm, float3 dx, float3 dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  uint w, h, d, m;
  Texture3D<float4> t = ResourceDescriptorHeap[im.srv]; t.GetDimensions(0, w, h, d, m);
  float3 a = dx * float3(w, h, d), b = dy * float3(w, h, d);
  return am_lod_of(max(dot(a, a), dot(b, b)), m);
}
// Cubes: direction gradients on the unit cube, whose face spans 2 units of `w` texels.
float2 am_lod_cube(AmImage im, AmSampler sm, float3 dx, float3 dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  uint w, h, e, m;
  if (AM_ARR(im)) { TextureCubeArray<float4> t = ResourceDescriptorHeap[im.srv]; t.GetDimensions(0, w, h, e, m); }
  else { TextureCube<float4> t1 = ResourceDescriptorHeap[im.srv]; t1.GetDimensions(0, w, h, m); }
  float3 a = dx * (0.5f * w), b = dy * (0.5f * w);
  return am_lod_of(max(dot(a, a), dot(b, b)), m);
}
)";
const char* kImageHelpers2 = R"(// Sampling with gradients (given, or taken across the console's quads of lanes).
float4 am_grad_1d(AmImage im, AmSampler sm, float2 c, float dx, float dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { Texture1DArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleGrad(s, c, dx, dy); }
  Texture1D<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleGrad(s, c.x, dx, dy);
}
float4 am_grad_2d(AmImage im, AmSampler sm, float3 c, float2 dx, float2 dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { Texture2DArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleGrad(s, c, dx, dy); }
  Texture2D<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleGrad(s, c.xy, dx, dy);
}
float4 am_grad_3d(AmImage im, AmSampler sm, float3 c, float3 dx, float3 dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  Texture3D<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleGrad(s, c, dx, dy);
}
float4 am_grad_cube(AmImage im, AmSampler sm, float4 c, float3 dx, float3 dy) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { TextureCubeArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleGrad(s, c, dx, dy); }
  TextureCube<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleGrad(s, c.xyz, dx, dy);
}
// Depth compares at level 0, with the S#'s comparison.
float am_cmp_lz_1d(AmImage im, AmSampler sm, float2 c, float ref) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerComparisonState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { Texture1DArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleCmpLevelZero(s, c, ref); }
  Texture1D<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleCmpLevelZero(s, c.x, ref);
}
float am_cmp_lz_2d(AmImage im, AmSampler sm, float3 c, float ref) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerComparisonState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { Texture2DArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleCmpLevelZero(s, c, ref); }
  Texture2D<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleCmpLevelZero(s, c.xy, ref);
}
float am_cmp_lz_cube(AmImage im, AmSampler sm, float4 c, float ref) {
  if (!AM_SAMPLED(im, sm)) return 0.0f;
  SamplerComparisonState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) { TextureCubeArray<float4> t = ResourceDescriptorHeap[im.srv]; return t.SampleCmpLevelZero(s, c, ref); }
  TextureCube<float4> t1 = ResourceDescriptorHeap[im.srv]; return t1.SampleCmpLevelZero(s, c.xyz, ref);
}
// Gathers one channel of the four texels a bilinear sample would use (D3D's order).
#define AM_GATHER(t, s, p, o) (comp == 0u ? t.GatherRed(s, p, o) : comp == 1u ? t.GatherGreen(s, p, o) : \
                               comp == 2u ? t.GatherBlue(s, p, o) : t.GatherAlpha(s, p, o))
#define AM_GATHER_NO(t, s, p) (comp == 0u ? t.GatherRed(s, p) : comp == 1u ? t.GatherGreen(s, p) : \
                               comp == 2u ? t.GatherBlue(s, p) : t.GatherAlpha(s, p))
uint4 am_gather_2d(AmImage im, AmSampler sm, float3 c, uint comp, int2 o) {
  if (!AM_SAMPLED(im, sm)) return AM_NONE;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) AM_CLASS_RET(Texture2DArray, im.srv, AM_GATHER(t, s, c, o))
  AM_CLASS_RET(Texture2D, im.srv, AM_GATHER(t, s, c.xy, o))
}
uint4 am_gather_cube(AmImage im, AmSampler sm, float4 c, uint comp) {
  if (!AM_SAMPLED(im, sm)) return AM_NONE;
  SamplerState s = SamplerDescriptorHeap[sm.index];
  if (AM_ARR(im)) AM_CLASS_RET(TextureCubeArray, im.srv, AM_GATHER_NO(t, s, c))
  AM_CLASS_RET(TextureCube, im.srv, AM_GATHER_NO(t, s, c.xyz))
}
// Size of a level: (width, height, depth or layers, levels).
uint4 am_info_1d(AmImage im, uint mip) {
  uint w = 0u, e = 1u, n = 0u;
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if (AM_ARR(im)) AM_CLASS_DO(Texture1DArray, im.srv, t.GetDimensions(mip, w, e, n))
  else AM_CLASS_DO(Texture1D, im.srv, t.GetDimensions(mip, w, n))
  return uint4(w, e, 1u, n);
}
uint4 am_info_2d(AmImage im, uint mip) {
  uint w = 0u, h = 0u, e = 1u, n = 0u;
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if (AM_ARR(im)) AM_CLASS_DO(Texture2DArray, im.srv, t.GetDimensions(mip, w, h, e, n))
  else AM_CLASS_DO(Texture2D, im.srv, t.GetDimensions(mip, w, h, n))
  return uint4(w, h, e, n);
}
uint4 am_info_3d(AmImage im, uint mip) {
  uint w = 0u, h = 0u, d = 0u, n = 0u;
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  AM_CLASS_DO(Texture3D, im.srv, t.GetDimensions(mip, w, h, d, n))
  return uint4(w, h, d, n);
}
uint4 am_info_cube(AmImage im, uint mip) {
  uint w = 0u, h = 0u, e = 1u, n = 0u;
  if ((im.flags & AM_VALID) == 0u) return AM_NONE;
  if (AM_ARR(im)) AM_CLASS_DO(TextureCubeArray, im.srv, t.GetDimensions(mip, w, h, e, n))
  else AM_CLASS_DO(TextureCube, im.srv, t.GetDimensions(mip, w, h, n))
  return uint4(w, h, e, n);
}
// 32-bit image atomics (the numbering of am_atomic); the value before.
#define AM_IMG_ATOMIC(r) \
  if (op == 0u) InterlockedExchange(r, x, o); else if (op == 1u) InterlockedCompareExchange(r, cmp, x, o); \
  else if (op == 2u) InterlockedAdd(r, x, o); else if (op == 3u) InterlockedAdd(r, 0u - x, o); \
  else if (op == 5u) InterlockedMin(r, x, o); else if (op == 7u) InterlockedMax(r, x, o); \
  else if (op == 8u) InterlockedAnd(r, x, o); else if (op == 9u) InterlockedOr(r, x, o); else InterlockedXor(r, x, o);
#define AM_IMG_ATOMIC_S(r) { int so = 0; if (op == 4u) InterlockedMin(r, asint(x), so); else InterlockedMax(r, asint(x), so); o = asuint(so); }
#define AM_ATOMIC_BODY(T, at) \
  if (op == 4u || op == 6u) { T<int> t = ResourceDescriptorHeap[im.uav]; AM_IMG_ATOMIC_S(t[at]) } \
  else { T<uint> t = ResourceDescriptorHeap[im.uav]; AM_IMG_ATOMIC(t[at]) }
uint am_atomic_1d(AmImage im, int4 c, uint op, uint x, uint cmp) {
  uint o = 0u;
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return o;
  if (AM_ARR(im)) { AM_ATOMIC_BODY(RWTexture1DArray, uint2(c.xy)) } else { AM_ATOMIC_BODY(RWTexture1D, uint(c.x)) }
  return o;
}
uint am_atomic_2d(AmImage im, int4 c, uint op, uint x, uint cmp) {
  uint o = 0u;
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return o;
  if (AM_ARR(im)) { AM_ATOMIC_BODY(RWTexture2DArray, uint3(c.xyz)) } else { AM_ATOMIC_BODY(RWTexture2D, uint2(c.xy)) }
  return o;
}
uint am_atomic_3d(AmImage im, int4 c, uint op, uint x, uint cmp) {
  uint o = 0u;
  if ((im.flags & (AM_VALID | 8u)) != (AM_VALID | 8u)) return o;
  AM_ATOMIC_BODY(RWTexture3D, uint3(c.xyz))
  return o;
}
)";

const char* kBufferHelpers = R"(
// A V#: base address (48 bits), stride (14 bits), records; returns the address and how many
// bytes from it are in bounds (raw buffers: stride 0 counts bytes).
void am_buffer_address(uint4 rs, uint index, uint offset, uint soffset, out uint64_t p, out uint lim) {
  uint64_t base = am_u64(rs.x, rs.y & 0xFFFFu);
  uint stride = (rs.y >> 16) & 0x3FFFu;
  uint records = rs.z;
  uint64_t at = uint64_t(index) * stride + offset;
  p = base + at + soffset;
  uint64_t range = stride == 0u ? uint64_t(records) : uint64_t(records) * stride;
  lim = at >= range ? 0u : uint(min(range - at, uint64_t(16)));
}

uint am_fmt_info(uint f) { return f < 78u ? am_formats[f] : 0u; }
uint am_fmt_width(uint info, uint k) { return (info >> (6u + 6u * k)) & 63u; }
uint am_fmt_bytes(uint info) {
  // Absent channels have width 0.
  return (am_fmt_width(info, 0u) + am_fmt_width(info, 1u) + am_fmt_width(info, 2u) + am_fmt_width(info, 3u)) >> 3;
}
// The 1 a missing W reads as: 1 for the integer types, 1.0 otherwise.
uint am_fmt_one(uint info) { uint t = info & 7u; return t == 4u || t == 5u ? 1u : 0x3F800000u; }

// One typed element: its channels as the shader sees them, after the V#'s selects (word 3, three
// bits a channel: 0, 1, or X..W as 4..7). Out of bounds reads zero.
// One channel of a typed element: its bits at `o` in `raw`, converted by the format's type.
uint am_fmt_chan(uint4 raw, uint info, uint k, uint o) {
  if (k >= ((info >> 3) & 7u)) return k == 3u ? am_fmt_one(info) : 0u;
  uint t = info & 7u, w = am_fmt_width(info, k);
  uint m = w == 32u ? 0xFFFFFFFFu : (1u << w) - 1u;
  uint x = (raw[o >> 5] >> (o & 31u)) & m;
  int sx = int(x << (32u - w)) >> (32u - w);
  if (t == 0u) return asuint(float(x) / float(m));
  if (t == 1u) return asuint(max(float(sx) / float(m >> 1), -1.0f));
  if (t == 2u) return asuint(float(x));
  if (t == 3u) return asuint(float(sx));
  if (t == 4u) return x;
  if (t == 5u) return asuint(sx);
  if (w == 32u) return x;
  return asuint(f16tof32(x << (15u - min(w, 15u))));  // half, or the unsigned 11/10-bit floats
}
uint am_fmt_select(uint4 c, uint info, uint sel) { return sel >= 4u ? c[sel - 4u] : sel == 1u ? am_fmt_one(info) : 0u; }
uint4 am_fmt_load(uint64_t p, uint lim, uint info, uint w3) {
  uint eb = am_fmt_bytes(info);
  if (eb == 0u || eb > lim) return uint4(0u, 0u, 0u, 0u);
  uint4 raw = uint4(0u, 0u, 0u, 0u);
  if (eb < 4u) {
    raw.x = am_load(p & ~uint64_t(3)) >> (uint(p & 3u) * 8u);
  } else {
    raw.x = am_load(p);
    if (eb >= 8u) raw.y = am_load(p + 4u);
    if (eb >= 12u) raw.z = am_load(p + 8u);
    if (eb >= 16u) raw.w = am_load(p + 12u);
  }
  uint w0 = am_fmt_width(info, 0u), w1 = am_fmt_width(info, 1u), w2 = am_fmt_width(info, 2u);
  uint4 c = uint4(am_fmt_chan(raw, info, 0u, 0u), am_fmt_chan(raw, info, 1u, w0), am_fmt_chan(raw, info, 2u, w0 + w1),
                  am_fmt_chan(raw, info, 3u, w0 + w1 + w2));
  return uint4(am_fmt_select(c, info, w3 & 7u), am_fmt_select(c, info, (w3 >> 3) & 7u), am_fmt_select(c, info, (w3 >> 6) & 7u),
               am_fmt_select(c, info, (w3 >> 9) & 7u));
}

// Writes the first `n` channels of one typed element.
// One channel's bits for a store, placed at `o` into data / mask.
void am_fmt_put(inout uint4 data, inout uint4 mask, uint info, uint k, uint o, uint n, uint value) {
  if (k >= ((info >> 3) & 7u) || k >= n) return;
  uint t = info & 7u, w = am_fmt_width(info, k);
  uint m = w == 32u ? 0xFFFFFFFFu : (1u << w) - 1u, h = m >> 1;
  float f = asfloat(value);
  uint x;
  if (t == 0u) x = uint(round(saturate(f) * float(m)));
  else if (t == 1u) x = uint(int(round(clamp(f, -1.0f, 1.0f) * float(h))));
  else if (t == 2u) x = min(am_cvt_u32(f), m);
  else if (t == 3u) x = uint(clamp(am_cvt_i32(f), -int(h) - 1, int(h)));
  else if (t == 4u) x = min(value, m);
  else if (t == 5u) x = uint(clamp(int(value), -int(h) - 1, int(h)));
  else if (w == 32u) x = value;
  else if (w == 16u) x = am_f16(f);
  else x = am_f16(max(f, 0.0f)) >> (15u - w);
  data[o >> 5] |= (x & m) << (o & 31u);
  mask[o >> 5] |= m << (o & 31u);
}
// Writes the first `n` channels of one typed element.
void am_fmt_store(uint64_t p, uint lim, uint info, uint n, uint4 val) {
  uint eb = am_fmt_bytes(info);
  if (eb == 0u || eb > lim) return;
  uint4 data = uint4(0u, 0u, 0u, 0u), mask = uint4(0u, 0u, 0u, 0u);
  uint w0 = am_fmt_width(info, 0u), w1 = am_fmt_width(info, 1u), w2 = am_fmt_width(info, 2u);
  am_fmt_put(data, mask, info, 0u, 0u, n, val.x);
  am_fmt_put(data, mask, info, 1u, w0, n, val.y);
  am_fmt_put(data, mask, info, 2u, w0 + w1, n, val.z);
  am_fmt_put(data, mask, info, 3u, w0 + w1 + w2, n, val.w);
  if (eb < 4u) {
    uint sh = uint(p & 3u) * 8u;
    am_store_masked(p & ~uint64_t(3), data.x << sh, mask.x << sh);
    return;
  }
  if (mask.x != 0u) am_store_masked(p, data.x, mask.x);
  if (eb >= 8u && mask.y != 0u) am_store_masked(p + 4u, data.y, mask.y);
  if (eb >= 12u && mask.z != 0u) am_store_masked(p + 8u, data.z, mask.z);
  if (eb >= 16u && mask.w != 0u) am_store_masked(p + 12u, data.w, mask.w);
}
)";

// The unified buffer formats (gfx10): bits 0-2 the type (unorm, snorm, uscaled, sscaled, uint,
// sint, float), 3-5 the channels, then six bits of width a channel, from bit 0 of the element up.
std::string format_table() {
  struct Group {
    uint32_t first;
    std::vector<uint32_t> widths;
    std::vector<uint32_t> types;
  };
  const std::vector<uint32_t> six = {0, 1, 2, 3, 4, 5}, seven = {0, 1, 2, 3, 4, 5, 6}, wide = {4, 5, 6};
  // The names give the channels from the top bit down: 10_11_11 is X 11 bits at bit 0.
  const Group groups[] = {{1, {8}, six},           {7, {16}, seven},           {14, {8, 8}, six},
                          {20, {32}, wide},        {23, {16, 16}, seven},      {30, {11, 11, 10}, seven},
                          {37, {10, 11, 11}, seven}, {44, {2, 10, 10, 10}, six}, {50, {10, 10, 10, 2}, six},
                          {56, {8, 8, 8, 8}, six}, {62, {32, 32}, wide},       {65, {16, 16, 16, 16}, seven},
                          {72, {32, 32, 32}, wide}, {75, {32, 32, 32, 32}, wide}};
  uint32_t v[78] = {};
  for (const Group& g : groups) {
    for (size_t k = 0; k < g.types.size(); ++k) {
      uint32_t x = g.types[k] | static_cast<uint32_t>(g.widths.size()) << 3;
      for (size_t j = 0; j < g.widths.size(); ++j) x |= g.widths[j] << (6 + 6 * j);
      v[g.first + k] = x;
    }
  }
  std::string out = "static const uint am_formats[78] = {";
  for (int i = 0; i < 78; ++i) out += (i ? ", " : "") + hex32(v[i]);
  return out + "};\n";
}

}  // namespace


// ---- control flow ----
//
// The program's blocks: their code (without the branch that ends them) and how they end.
struct CfgBlock {
  uint32_t start = 0;  // dword
  std::string code;
  enum End : uint8_t { Fall, Jump, Cond, Stop } end = Fall;
  std::string cond;    // Cond: the branch is taken when this holds
  uint32_t target = 0; // Jump, Cond: the branch's target (dword)
  uint32_t next = 0;   // Fall, Cond: the dword after the block
};

// Re-indents a block's lines (emitted at six spaces) to `indent`.
std::string indented(const std::string& code, const std::string& indent) {
  std::string out;
  size_t at = 0;
  while (at < code.size()) {
    size_t end = code.find('\n', at);
    if (end == std::string::npos) end = code.size();
    std::string_view line(code.data() + at, end - at);
    if (line.substr(0, 6) == "      ") line.remove_prefix(6);
    out += indent;
    out += line;
    out += '\n';
    at = end + 1;
  }
  return out;
}

// Structured control flow from a reducible CFG, after Ramsey's "Beyond Relooper": a node's
// children in the dominator tree that are merge points (two or more forward edges in) follow
// it, each after a block that the branches to it leave; a loop header opens a loop that its
// back edges continue; any other forward branch inlines its target. In HLSL a block is
// `switch (0u) { default: }` (break leaves it; LLVM sees no loop) and a loop `for (;;)`; a
// branch past more than the innermost of them
// sets am_br to the target's depth and unwinds.
class Structurer {
 public:
  explicit Structurer(const std::vector<CfgBlock>& blocks) : b_(blocks) {}

  // False if the CFG is not reducible (or not what we expect); the caller then dispatches.
  bool run(std::string& out, bool& uses_br) {
    const int n = static_cast<int>(b_.size());
    std::unordered_map<uint32_t, int> index;
    for (int i = 0; i < n; ++i) index[b_[i].start] = i;
    succ_.assign(n, {});
    for (int i = 0; i < n; ++i) {
      const auto to = [&](uint32_t dword) {
        const auto it = index.find(dword);
        return it == index.end() ? -1 : it->second;
      };
      const CfgBlock& x = b_[i];
      if (x.end == CfgBlock::Fall) succ_[i] = {to(x.next)};
      else if (x.end == CfgBlock::Jump) succ_[i] = {to(x.target)};
      else if (x.end == CfgBlock::Cond) succ_[i] = {to(x.next), to(x.target)};
      for (int s : succ_[i]) {
        if (s < 0) return false;
      }
    }
    // Reverse postorder from the entry (iterative DFS).
    rpo_.assign(n, -1);
    std::vector<int> post, state(n, 0), it(n, 0), stack{0};
    state[0] = 1;
    while (!stack.empty()) {
      const int x = stack.back();
      if (it[x] < static_cast<int>(succ_[x].size())) {
        const int y = succ_[x][it[x]++];
        if (state[y] == 0) {
          state[y] = 1;
          stack.push_back(y);
        }
      } else {
        post.push_back(x);
        stack.pop_back();
      }
    }
    order_.assign(post.rbegin(), post.rend());
    for (size_t k = 0; k < order_.size(); ++k) rpo_[order_[k]] = static_cast<int>(k);
    // Dominators (Cooper, Harvey and Kennedy).
    std::vector<std::vector<int>> pred(n);
    for (int i = 0; i < n; ++i) {
      if (rpo_[i] < 0) continue;
      for (int s : succ_[i]) pred[s].push_back(i);
    }
    idom_.assign(n, -1);
    idom_[0] = 0;
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t k = 1; k < order_.size(); ++k) {
        const int x = order_[k];
        int d = -1;
        for (int p : pred[x]) {
          if (idom_[p] < 0) continue;
          if (d < 0) {
            d = p;
            continue;
          }
          int a = p, c = d;
          while (a != c) {
            while (rpo_[a] > rpo_[c]) a = idom_[a];
            while (rpo_[c] > rpo_[a]) c = idom_[c];
          }
          d = a;
        }
        if (d != idom_[x]) {
          idom_[x] = d;
          changed = true;
        }
      }
    }
    // Back edges must go to a dominator (reducible); forward edges count toward merges.
    header_.assign(n, false);
    merge_.assign(n, false);
    std::vector<int> forward_in(n, 0);
    for (int i = 0; i < n; ++i) {
      if (rpo_[i] < 0) continue;
      for (int s : succ_[i]) {
        if (rpo_[s] <= rpo_[i]) {
          if (!dominates(s, i)) return false;
          header_[s] = true;
        } else {
          ++forward_in[s];
        }
      }
    }
    children_.assign(n, {});
    for (size_t k = 1; k < order_.size(); ++k) {
      const int x = order_[k];
      merge_[x] = forward_in[x] >= 2;
      children_[idom_[x]].push_back(x);
    }
    ok_ = true;
    do_tree(0);
    if (!ok_) return false;
    out = std::move(out_);
    uses_br = uses_br_;
    return true;
  }

 private:
  enum class Kind : uint8_t { If, Loop, Block };
  struct Frame {
    Kind kind;
    int node;              // Loop: its header; Block: the node that follows it
    bool unwound = false;  // a branch passes out through it: check am_br after it
  };

  bool dominates(int a, int b) const {
    for (int x = b;; x = idom_[x]) {
      if (x == a) return true;
      if (x == 0 || idom_[x] < 0) return false;
    }
  }
  std::string indent() const { return std::string(2 * (frames_.size() + 1), ' '); }
  void line(const std::string& text) { out_ += indent() + text + "\n"; }
  int depth_of(size_t f) const {  // 1-based depth among loops and blocks
    int d = 0;
    for (size_t k = 0; k <= f; ++k) d += frames_[k].kind != Kind::If;
    return d;
  }

  void do_tree(int x) {
    std::vector<int> ys;
    for (int c : children_[x]) {
      if (merge_[c]) ys.push_back(c);
    }
    std::sort(ys.begin(), ys.end(), [&](int a, int c) { return rpo_[a] > rpo_[c]; });
    if (header_[x]) {
      line("[loop] for (;;) {");
      frames_.push_back({Kind::Loop, x});
      node_within(x, ys, 0);
      line("break;");
      close_loop_like("}");
    } else {
      node_within(x, ys, 0);
    }
  }

  void node_within(int x, const std::vector<int>& ys, size_t i) {
    if (!ok_) return;
    if (i < ys.size()) {
      line("switch (0u) { default:");
      frames_.push_back({Kind::Block, ys[i]});
      node_within(x, ys, i + 1);
      line("break;");
      close_loop_like("}");
      do_tree(ys[i]);
      return;
    }
    const CfgBlock& blk = b_[x];
    out_ += indented(blk.code, indent());
    switch (blk.end) {
      case CfgBlock::Stop: break;
      case CfgBlock::Fall:
      case CfgBlock::Jump: do_branch(x, succ_[x][0]); break;
      case CfgBlock::Cond:
        if (succ_[x][0] == succ_[x][1]) {
          do_branch(x, succ_[x][0]);
          break;
        }
        line("if (" + blk.cond + ") {");
        frames_.push_back({Kind::If, -1});
        do_branch(x, succ_[x][1]);
        frames_.pop_back();
        line("} else {");
        frames_.push_back({Kind::If, -1});
        do_branch(x, succ_[x][0]);
        frames_.pop_back();
        line("}");
        break;
    }
  }

  void do_branch(int from, int to) {
    if (rpo_[to] <= rpo_[from]) return branch_to(Kind::Loop, to);
    if (merge_[to]) return branch_to(Kind::Block, to);
    if (idom_[to] != from) {
      ok_ = false;
      return;
    }
    do_tree(to);
  }

  // A branch to the loop headed by `node` (continue) or out of the block that `node` follows.
  void branch_to(Kind kind, int node) {
    int f = -1, inner = -1;
    for (int k = static_cast<int>(frames_.size()) - 1; k >= 0; --k) {
      if (frames_[k].kind != Kind::If && inner < 0) inner = k;
      if (frames_[k].kind == kind && frames_[k].node == node) {
        f = k;
        break;
      }
    }
    if (f < 0) {
      ok_ = false;
      return;
    }
    if (f == inner) {
      line(kind == Kind::Loop ? "continue;" : "break;");
      return;
    }
    for (size_t k = f + 1; k < frames_.size(); ++k) {
      if (frames_[k].kind != Kind::If) frames_[k].unwound = true;
    }
    uses_br_ = true;
    line("am_br = " + std::to_string(depth_of(f)) + "u; break;");
  }

  // Closes the innermost loop or block; after it, a branch unwinding through it either has
  // arrived (the construct now innermost is its target) or goes on outward.
  void close_loop_like(const char* closing) {
    const Frame f = frames_.back();
    frames_.pop_back();
    line(closing);
    if (!f.unwound) return;
    int inner = -1;
    for (int k = static_cast<int>(frames_.size()) - 1; k >= 0; --k) {
      if (frames_[k].kind != Kind::If) {
        inner = k;
        break;
      }
    }
    if (inner < 0) {
      ok_ = false;
      return;
    }
    line("if (am_br != 0u) {");
    line("  if (am_br == " + std::to_string(depth_of(inner)) + "u) { am_br = 0u; " +
         (frames_[inner].kind == Kind::Loop ? "continue; }" : "break; }"));
    line("  break;");
    line("}");
  }

  const std::vector<CfgBlock>& b_;
  std::vector<std::vector<int>> succ_, children_;
  std::vector<int> rpo_, order_, idom_;
  std::vector<bool> header_, merge_;
  std::vector<Frame> frames_;
  std::string out_;
  bool ok_ = true, uses_br_ = false;
};

// The fallback: one loop over a switch on the block's start.
std::string dispatch(const std::vector<CfgBlock>& blocks) {
  std::string body;
  for (const CfgBlock& b : blocks) {
    body += "    case " + num(b.start) + "u:\n" + b.code;
    switch (b.end) {
      case CfgBlock::Fall: body += "      pc = " + num(b.next) + "u; break;\n"; break;
      case CfgBlock::Jump: body += "      pc = " + num(b.target) + "u; break;\n"; break;
      case CfgBlock::Cond:
        body += "      if (" + b.cond + ") { pc = " + num(b.target) + "u; break; }\n";
        body += "      pc = " + num(b.next) + "u; break;\n";
        break;
      case CfgBlock::Stop: break;
    }
  }
  return body;
}

// After an instruction: the descriptor lookups whose SGPRs it may have written are dropped.
// Vector, memory and image instructions write none (VOPC and VOP2 carries: VCC, EXEC only);
// SOPC and SOPP write SCC at most; the rest are taken to write any.
void forget_written(Ctx& c, const rd::Instruction& insn) {
  const std::string_view name = insn.info->name;
  switch (insn.encoding) {
    case rd::Encoding::Vop1:
    case rd::Encoding::Vop2:
    case rd::Encoding::Vopc:
      if (name == "v_readfirstlane_b32") break;
      c.drop_descriptors(106, 107);
      c.drop_descriptors(126, 127);
      return;
    case rd::Encoding::Vop3:
      if ((insn.info->flags & rd::kSdst) || name.substr(0, 5) == "v_cmp" || name.substr(0, 10) == "v_readlane" ||
          name == "v_readfirstlane_b32") {
        break;
      }
      return;
    case rd::Encoding::Vop3p:
    case rd::Encoding::Mimg:
    case rd::Encoding::Mubuf:
    case rd::Encoding::Mtbuf:
    case rd::Encoding::Ds:
    case rd::Encoding::Sopc:
    case rd::Encoding::Sopp:
      return;
    default:
      if (insn.encoding == rd::Encoding::Flat || name.substr(0, 7) == "global_") return;
      break;
  }
  c.live_images.clear();
  c.live_samplers.clear();
}

// Lane helpers that read the register file (EXEC, VCC), for every stage.
std::string lane_macros(bool wave64) {
  std::string h;
  if (wave64) {
    h += "#define AM_ACT ((((am_lane < 32u) ? s[126] : s[127]) >> (am_lane & 31u)) & 1u)\n";
    h += "#define am_exec_zero() ((s[126] | s[127]) == 0u)\n#define am_vcc_zero() ((s[106] | s[107]) == 0u)\n";
    h += "#define am_lane_bit(lo, hi) (((((am_lane < 32u) ? (lo) : (hi)) >> (am_lane & 31u)) & 1u) != 0u)\n";
    h += "#define am_first_active() (s[126] != 0u ? firstbitlow(s[126]) : s[127] != 0u ? 32u + firstbitlow(s[127]) : 0u)\n";
    h += "#define am_lanes_below_lo() (am_lane >= 32u ? 0xFFFFFFFFu : (1u << am_lane) - 1u)\n";
    h += "#define am_lane_on(l) (((((l) < 32u ? s[126] : s[127]) >> ((l) & 31u)) & 1u) != 0u)\n";
    h += "#define am_lanes_below_hi() (am_lane < 32u ? 0u : (1u << (am_lane - 32u)) - 1u)\n";
  } else {
    h += "#define AM_ACT ((s[126] >> am_lane) & 1u)\n";
    h += "#define am_exec_zero() (s[126] == 0u)\n#define am_vcc_zero() (s[106] == 0u)\n";
    h += "#define am_lane_bit(lo, hi) ((((lo) >> am_lane) & 1u) != 0u)\n";
    h += "#define am_first_active() (s[126] != 0u ? firstbitlow(s[126]) : 0u)\n";
    h += "#define am_lanes_below_lo() ((1u << am_lane) - 1u)\n#define am_lanes_below_hi() 0u\n";
    h += "#define am_lane_on(l) (((s[126] >> ((l) & 31u)) & 1u) != 0u)\n";
  }
  return h;
}

// A vertex or pixel shader: its interface as HLSL structs, a body function holding the
// register file, and an entry that calls it. The host runs the lanes at its own wave size: the
// emulated EXEC covers the lanes the host wave has.
Translation finish_graphics(const TranslateInput& in, const Ctx& c, Translation tr, const std::string& body, bool uses_br,
                            uint32_t nv, Binding binding) {
  const bool vs = in.stage == HwStage::Vs;
  if (c.uses_lds) {
    tr.failure = "LDS in a vertex or pixel shader";
    tr.unsupported[tr.failure] = 1;
    return tr;
  }
  tr.profile = vs ? "vs_6_6" : "ps_6_6";
  std::string h;
  h += "// Translated by Project Americas' shader translator, version " + num(kTranslatorVersion) + ".\n";
  h += kHelpers;
  h += binding == Binding::Runtime ? kRuntimeBinding : kTestBinding;
  h += format_table();
  h += kBufferHelpers;
  if (c.uses_images) {
    h += kImageHelpers;
    if (c.uses_lod) h += kLodHelpers;
    h += kImageHelpers2;
  }
  h += "\nstruct AmIn {\n";
  if (!vs) h += "  float4 am_pos : SV_Position;\n";
  for (size_t k = 0; k < in.inputs.size(); ++k) {
    const bool per_vertex = !vs && c.per_vertex_attrs.count(static_cast<uint32_t>(k));
    h += std::string("  ") + (per_vertex ? "nointerpolation " : "") + "float4 i" + num(static_cast<uint32_t>(k)) + " : " +
         in.inputs[k].semantic + num(in.inputs[k].semantic_index) + ";\n";
  }
  // Last, so the attributes keep their registers.
  if (!vs && !c.per_vertex_attrs.empty()) h += "  float3 am_bary_in : SV_Barycentrics;\n";
  h += "};\n";
  if (!vs) {
    // An attribute's value: interpolated by the host, or here from its three vertices.
    if (!c.per_vertex_attrs.empty()) h += "static float3 am_bary;\n";
    for (size_t k = 0; k < in.inputs.size(); ++k) {
      const std::string n = num(static_cast<uint32_t>(k));
      if (c.per_vertex_attrs.count(static_cast<uint32_t>(k))) {
        h += "static uint4 am_vtx" + n + "[3];\n";
        h += "#define AM_IN" + n + " (am_bary.x * asfloat(am_vtx" + n + "[0]) + am_bary.y * asfloat(am_vtx" + n +
             "[1]) + am_bary.z * asfloat(am_vtx" + n + "[2]))\n";
      } else {
        h += "#define AM_IN" + n + " am_in.i" + n + "\n";
      }
    }
  }
  h += "struct AmOut {\n";
  if (vs) h += "  float4 pos : SV_Position;\n";
  for (size_t k = 0; k < in.outputs.size(); ++k) {
    h += "  float4 o" + num(static_cast<uint32_t>(k)) + " : " + in.outputs[k].semantic + num(in.outputs[k].semantic_index) + ";\n";
  }
  if (!vs && c.depth_export) h += "  float am_depth : SV_Depth;\n";
  h += "};\n";
  h += lane_macros(c.wave64);
  h += vs ? "#define AM_END() {}\n" : "#define AM_END() { if (AM_ACT == 0u) discard; }\n";
  h += vs ? "void am_body(AmIn am_in, uint am_vid, uint am_iid, inout AmOut am_o) {\n" : "void am_body(AmIn am_in, inout AmOut am_o) {\n";
  h += "  uint s[128];\n  uint v[" + num(nv) + "];\n" + (c.uses_movrel ? "  const uint am_nv = " + num(nv) + "u;\n" : std::string()) + " ";
  for (uint32_t i = 0; i < 128; ++i) h += " s[" + num(i) + "] = 0u;";
  h += "\n ";
  for (uint32_t j = 0; j < nv; ++j) h += " v[" + num(j) + "] = 0u;";
  h += "\n";
  h += "  uint am_null = 0u;\n  bool scc = false;\n  uint am_lane = WaveGetLaneIndex();\n";
  h += "  uint am_dpp = 0u;\n  bool am_dpp_on = true;\n";
  for (const uint32_t r : c.image_vars) h += "  AmImage am_im_" + num(r) + " = (AmImage)0;\n";
  for (const uint32_t r : c.sampler_vars) h += "  AmSampler am_sm_" + num(r) + " = (AmSampler)0;\n";
  for (uint32_t i = 0; i < std::min<uint32_t>(in.user_sgprs, binding == Binding::Runtime ? 20u : 32u); ++i) {
    h += "  s[" + num(i) + "] = am_user[" + num(i / 4) + "][" + num(i % 4) + "];\n";
  }
  if (vs) {
    h += "  v[0] = am_vid;\n  v[3] = am_iid;\n";
    for (size_t k = 0; k < in.inputs.size(); ++k) {
      for (uint32_t ch = 0; ch < std::min<uint32_t>(in.inputs[k].components, 4); ++ch) {
        const uint32_t r = in.inputs[k].vgpr + ch;
        if (r < nv) h += "  v[" + num(r) + "] = asuint(am_in.i" + num(static_cast<uint32_t>(k)) + "[" + num(ch) + "]);\n";
      }
    }
  }
  h += "  { uint4 b = WaveActiveBallot(true); s[126] = b.x; s[127] = " + std::string(c.wave64 ? "b.y" : "0u") + "; }\n";
  const std::string merged = merge_lane_guards(body);
  if (tr.structured) {
    if (uses_br) h += "  uint am_br = 0u;\n";
    h += merged;
    h += "}\n";
  } else {
    h += "  uint pc = 0u;\n  [loop] for (;;) {\n    [forcecase] switch (pc) {\n";
    h += merged;
    h += "    default:\n      return;\n    }\n  }\n}\n";
  }
  if (vs) {
    h += "AmOut main(AmIn am_in, uint am_vid : SV_VertexID, uint am_iid : SV_InstanceID) {\n";
    h += "  AmOut o = (AmOut)0;\n  am_body(am_in, am_vid, am_iid, o);\n  return o;\n}\n";
  } else {
    std::string fetch;
    if (!c.per_vertex_attrs.empty()) fetch += "  am_bary = am_in.am_bary_in;\n";
    for (uint32_t k : c.per_vertex_attrs) {
      for (int vtx = 0; vtx < 3; ++vtx) {
        fetch += "  am_vtx" + num(k) + "[" + std::to_string(vtx) + "] = asuint(GetAttributeAtVertex(am_in.i" + num(k) + ", " +
                 std::to_string(vtx) + "));\n";
      }
    }
    // A lane that exports no depth keeps the rasterized one.
    if (c.depth_export) fetch += "  o.am_depth = am_in.am_pos.z;\n";
    h += "AmOut main(AmIn am_in) {\n  AmOut o = (AmOut)0;\n" + fetch + "  am_body(am_in, o);\n  return o;\n}\n";
  }
  tr.hlsl = std::move(h);
  tr.ok = true;
  return tr;
}

Translation translate(const TranslateInput& in, Binding binding) {
  Translation tr;
  tr.wave_size = in.wave32 ? 32 : 64;
  const bool graphics = in.stage == HwStage::Vs || in.stage == HwStage::Ps;
  if (in.stage != HwStage::Cs && !graphics) {
    tr.failure = std::string("stage ") + hw_stage_name(in.stage) + " (compute, vertex and pixel only so far)";
    tr.unsupported[tr.failure] = 1;
    return tr;
  }

  const rd::ProgramScan scan = rd::scan_program(in.code);
  if (scan.errors) {
    tr.failure = "decoding: " + scan.first_error_reason;
    tr.unsupported[tr.failure] = 1;
    return tr;
  }
  // A vertex shader's s_swappc_b64 calls the console's vertex fetch, which returns: its
  // attributes are inputs here.
  if (scan.indirect && (scan.setpc || in.stage != HwStage::Vs)) {
    tr.failure = "indirect jumps (s_setpc_b64)";
    tr.unsupported[tr.failure] = 1;
    return tr;
  }

  // Block starts: the entry, branch targets, and whatever follows a branch or s_endpgm.
  std::set<uint32_t> starts{0};
  for (uint32_t t : scan.targets) starts.insert(t);
  for (size_t k = 0; k + 1 < scan.starts.size(); ++k) {
    rd::Instruction insn;
    rd::decode(in.code, scan.starts[k], insn);
    if (insn.info->flags & (rd::kBranch | rd::kTerminator)) starts.insert(scan.starts[k + 1]);
  }
  Ctx c;
  c.input = &in;
  c.wave64 = !in.wave32;
  std::vector<CfgBlock> blocks;
  const auto& ops = table();
  for (size_t k = 0; k < scan.starts.size(); ++k) {
    const uint32_t at = scan.starts[k];
    rd::Instruction insn;
    rd::decode(in.code, at, insn);
    const uint32_t next = at + insn.dwords;
    if (starts.count(at) || blocks.empty()) {
      if (!blocks.empty() && blocks.back().end == CfgBlock::Fall) blocks.back().next = at;
      blocks.emplace_back();
      blocks.back().start = at;
      c.live_images.clear();
      c.live_samplers.clear();
    }
    CfgBlock& blk = blocks.back();
    c.in = &insn;
    c.pc = at;
    c.next = next;
    c.out.clear();
    c.dpp = false;
    const std::string name = insn.info->name;
    const uint16_t flags = insn.info->flags;
    bool ok = true;
    if (flags & rd::kTerminator) {
      c.line("AM_END(); return;");
      blk.end = CfgBlock::Stop;
    } else if (name == "s_swappc_b64" && in.stage == HwStage::Vs) {
      // The vertex fetch: the attributes are in place already; the return address is the
      // program's base for its own data (our code starts at address 0).
      c.set_s64(c.f(Field::SDST), "uint64_t(" + num(uint64_t{next} * 4) + ")");
    } else if (flags & rd::kBranch) {
      const int64_t target = static_cast<int64_t>(next) + static_cast<int16_t>(insn.field(Field::SIMM16));
      std::string cond;
      if (name == "s_branch") cond = "true";
      else if (name == "s_cbranch_scc0") cond = "!scc";
      else if (name == "s_cbranch_scc1") cond = "scc";
      else if (name == "s_cbranch_vccz") cond = "am_vcc_zero()";
      else if (name == "s_cbranch_vccnz") cond = "!am_vcc_zero()";
      else if (name == "s_cbranch_execz") cond = "am_exec_zero()";
      else if (name == "s_cbranch_execnz") cond = "!am_exec_zero()";
      else ok = c.fail(name);
      if (ok) {
        blk.target = static_cast<uint32_t>(target);
        blk.next = next;
        if (name == "s_branch") blk.end = CfgBlock::Jump;
        else {
          blk.end = CfgBlock::Cond;
          blk.cond = cond;
        }
      }
    } else {
      const auto it = ops.find(name);
      if (it == ops.end()) ok = c.fail(name);
      else ok = it->second(c);
    }
    if (!ok || !c.failure.empty()) {
      const std::string why = c.failure.empty() ? name : c.failure;
      ++tr.unsupported[why];
      if (tr.failure.empty()) tr.failure = why;
      c.failure.clear();
      continue;
    }
    blk.code += c.out;
    forget_written(c, insn);
  }
  // Running off the end of the code ends the program.
  if (!blocks.empty() && blocks.back().end == CfgBlock::Fall) {
    blocks.back().end = CfgBlock::Stop;
    blocks.back().code += "      return;\n";
  }
  if (!tr.unsupported.empty()) return tr;
  std::string body;
  bool uses_br = false;
  tr.structured = Structurer(blocks).run(body, uses_br);
  if (!tr.structured) body = dispatch(blocks);

  const uint32_t nv = std::max<uint32_t>({c.max_vgpr, in.vgprs ? in.vgprs : 1u, 4u});
  tr.vgpr_count = nv;
  if (graphics) return finish_graphics(in, c, std::move(tr), body, uses_br, nv, binding);
  const uint32_t user = (in.cs_rsrc2 >> 1) & 31;
  const uint32_t tid_comps = (in.cs_rsrc2 >> 11) & 3;
  std::string h;
  h += "// Translated by Project Americas' shader translator, version " + num(kTranslatorVersion) + ".\n";
  h += kHelpers;
  h += binding == Binding::Runtime ? kRuntimeBinding : kTestBinding;
  h += format_table();
  h += kBufferHelpers;
  if (c.uses_images) {
    h += kImageHelpers;
    if (c.uses_lod) h += kLodHelpers;
    h += kImageHelpers2;
  }
  if (c.uses_lds || in.lds_bytes) h += "groupshared uint am_lds[" + num(std::max<uint32_t>(in.lds_bytes / 4, 1)) + "];\n";
  const uint32_t tx = std::max<uint32_t>(in.cs_threads[0], 1), ty = std::max<uint32_t>(in.cs_threads[1], 1),
                 tz = std::max<uint32_t>(in.cs_threads[2], 1);
  h += "\n[numthreads(" + num(tx) + ", " + num(ty) + ", " + num(tz) + ")]\n[WaveSize(" + num(tr.wave_size) + ")]\n";
  h += "void main(uint3 am_tid : SV_GroupThreadID, uint3 am_gid : SV_GroupID, uint am_flat : SV_GroupIndex) {\n";
  h += "  uint s[128];\n  uint v[" + num(nv) + "];\n" + (c.uses_movrel ? "  const uint am_nv = " + num(nv) + "u;\n" : std::string()) + " ";
  for (uint32_t i = 0; i < 128; ++i) h += " s[" + num(i) + "] = 0u;";
  h += "\n ";
  for (uint32_t j = 0; j < nv; ++j) h += " v[" + num(j) + "] = 0u;";
  h += "\n";
  h += "  uint am_null = 0u;\n  bool scc = false;\n  uint am_lane = WaveGetLaneIndex();\n";
  h += "  uint am_dpp = 0u;\n  bool am_dpp_on = true;\n";
  for (const uint32_t r : c.image_vars) h += "  AmImage am_im_" + num(r) + " = (AmImage)0;\n";
  for (const uint32_t r : c.sampler_vars) h += "  AmSampler am_sm_" + num(r) + " = (AmSampler)0;\n";
  for (uint32_t i = 0; i < (binding == Binding::Runtime ? std::min(user, 20u) : user); ++i) h += "  s[" + num(i) + "] = am_user[" + num(i / 4) + "][" + num(i % 4) + "];\n";
  uint32_t next_s = user;
  for (int k = 0; k < 3; ++k) {
    if ((in.cs_rsrc2 >> (7 + k)) & 1) h += "  s[" + num(next_s++) + "] = am_gid[" + num(k) + "];\n";
  }
  if ((in.cs_rsrc2 >> 10) & 1) h += "  s[" + num(next_s++) + "] = 0u;  // thread-group info: not filled yet\n";
  h += "  v[0] = am_tid.x;\n";
  if (tid_comps >= 1) h += "  v[1] = am_tid.y;\n";
  if (tid_comps >= 2) h += "  v[2] = am_tid.z;\n";
  h += "  { uint4 b = WaveActiveBallot(true); s[126] = b.x; s[127] = " + std::string(c.wave64 ? "b.y" : "0u") + "; }\n";
  // Lane helpers that read the register file.
  if (c.wave64) {
    h += "#define AM_ACT ((((am_lane < 32u) ? s[126] : s[127]) >> (am_lane & 31u)) & 1u)\n";
    h += "#define am_exec_zero() ((s[126] | s[127]) == 0u)\n#define am_vcc_zero() ((s[106] | s[107]) == 0u)\n";
    h += "#define am_lane_bit(lo, hi) (((((am_lane < 32u) ? (lo) : (hi)) >> (am_lane & 31u)) & 1u) != 0u)\n";
    h += "#define am_first_active() (s[126] != 0u ? firstbitlow(s[126]) : s[127] != 0u ? 32u + firstbitlow(s[127]) : 0u)\n";
    h += "#define am_lanes_below_lo() (am_lane >= 32u ? 0xFFFFFFFFu : (1u << am_lane) - 1u)\n";
    h += "#define am_lane_on(l) (((((l) < 32u ? s[126] : s[127]) >> ((l) & 31u)) & 1u) != 0u)\n";
    h += "#define am_lanes_below_hi() (am_lane < 32u ? 0u : (1u << (am_lane - 32u)) - 1u)\n";
  } else {
    h += "#define AM_ACT ((s[126] >> am_lane) & 1u)\n";
    h += "#define am_exec_zero() (s[126] == 0u)\n#define am_vcc_zero() (s[106] == 0u)\n";
    h += "#define am_lane_bit(lo, hi) ((((lo) >> am_lane) & 1u) != 0u)\n";
    h += "#define am_first_active() (s[126] != 0u ? firstbitlow(s[126]) : 0u)\n";
    h += "#define am_lanes_below_lo() ((1u << am_lane) - 1u)\n#define am_lanes_below_hi() 0u\n";
    h += "#define am_lane_on(l) (((s[126] >> ((l) & 31u)) & 1u) != 0u)\n";
  }
  // At s_endpgm the tests' binding dumps the registers; the runtime's has nothing to do.
  if (binding == Binding::Runtime) {
    h += "#define AM_END() {}\n";
  } else {
    // At s_endpgm the tests' binding dumps the registers (written out: no loop for LLVM).
    std::string d = "#define AM_END() { uint am_at = " + num(kDumpLanes) + "u + (am_gid.x * " + num(tx * ty * tz) +
                    "u + am_flat) * " + num(nv * 4) + "u;";
    for (uint32_t k = 0; k < nv; ++k) d += " am_dump.Store(am_at + " + num(4 * k) + "u, v[" + num(k) + "]);";
    d += " if (am_flat == 0u && all(am_gid == 0u)) {";
    for (uint32_t q = 0; q < 128; ++q) d += " am_dump.Store(" + num(kDumpSgprs + 4 * q) + "u, s[" + num(q) + "]);";
    d += " am_dump.Store(" + num(kDumpSgprs + 512) + "u, scc ? 1u : 0u); } }\n";
    h += d;
  }
  body = merge_lane_guards(body);
  if (tr.structured) {
    if (uses_br) h += "  uint am_br = 0u;\n";
    h += body;
    h += "}\n";
  } else {
    h += "  uint pc = 0u;\n  [loop] for (;;) {\n    [forcecase] switch (pc) {\n";
    h += body;
    h += "    default:\n      return;\n    }\n  }\n}\n";
  }
  tr.hlsl = std::move(h);
  tr.ok = true;
  return tr;
}

}  // namespace am::shader
