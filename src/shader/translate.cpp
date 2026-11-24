#include "shader/translate.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string_view>
#include <unordered_map>

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
  bool wave64 = true;
  uint32_t pc = 0;    // dword index of this instruction
  uint32_t next = 0;  // and of the next
  std::string failure;
  bool uses_lds = false;
  uint32_t max_vgpr = 0;

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
  // A masked VGPR write.
  void vwrite(uint32_t reg, const std::string& expr) { line("if (AM_ACT) " + vdst(reg) + " = " + expr + ";"); }
};

// ---- vector operand access by instruction shape ----

bool is_vop3(const rd::Instruction& i) { return i.encoding == rd::Encoding::Vop3; }

// Source k of a vector ALU instruction (VOP1/2/C, or VOP3).
std::string vsrc(Ctx& c, int k, bool f16 = false) {
  const rd::Instruction& i = *c.in;
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
  if (is_vop3(*c.in)) {
    if ((c.f(Field::ABS) >> k) & 1) x = "abs(" + x + ")";
    if ((c.f(Field::NEG) >> k) & 1) x = "(-" + x + ")";
  }
  return x;
}
uint32_t vop_dst(Ctx& c) { return c.f(Field::VDST); }
// A float result with VOP3's output modifier and clamp.
std::string fresult(Ctx& c, const std::string& expr) {
  std::string r = expr;
  if (is_vop3(*c.in)) {
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
bool no_sdwa_dpp(Ctx& c) {
  if (c.in->form != rd::Form::Plain) return c.fail(std::string(c.in->info->name) + " (sdwa/dpp)");
  if (is_vop3(*c.in) && c.f(Field::OP_SEL)) return c.fail(std::string(c.in->info->name) + " (op_sel)");
  return true;
}

// ---- the instruction table ----

using Handler = std::function<bool(Ctx&)>;

void float_unary(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!no_sdwa_dpp(c)) return false;
    c.vwrite(vop_dst(c), fresult(c, fill(body, fsrc(c, 0))));
    return true;
  };
}
void float_binary(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!no_sdwa_dpp(c)) return false;
    const std::string a = fsrc(c, 0), b = fsrc(c, 1);
    c.vwrite(vop_dst(c), fresult(c, fill(body, a, b)));
    return true;
  };
}
void float_ternary(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!no_sdwa_dpp(c) || !is_vop3(*c.in)) return c.fail(std::string(c.in->info->name));
    const std::string a = fsrc(c, 0), b = fsrc(c, 1), d = fsrc(c, 2);
    c.vwrite(vop_dst(c), fresult(c, fill(body, a, b, d)));
    return true;
  };
}
// Integer forms: A, B, C are uint sources; `vop3_only` for three-source ones.
void int_op(std::unordered_map<std::string, Handler>& t, const char* name, const char* body) {
  t[name] = [body](Ctx& c) {
    if (!no_sdwa_dpp(c)) return false;
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
             bool is64 = false) {
  for (const bool x : {false, true}) {
    const std::string full = (x ? "v_cmpx_" : "v_cmp_") + name;
    t[full] = [cond, fl, x, is64](Ctx& c) {
      if (!no_sdwa_dpp(c)) return false;
      std::string a, b;
      if (is64) {
        a = c.src64(c.f(Field::SRC0));
        b = is_vop3(*c.in) ? c.src64(c.f(Field::SRC1)) : "am_u64(v[" + num(c.f(Field::VSRC1)) + "], v[" + num(c.f(Field::VSRC1) + 1) + "])";
      } else if (fl) {
        a = fsrc(c, 0);
        b = fsrc(c, 1);
      } else {
        a = vsrc(c, 0);
        b = vsrc(c, 1);
      }
      c.line("{ uint4 m = WaveActiveBallot((" + fill(cond, a, b) + ") && AM_ACT != 0u);");
      const uint32_t dst = is_vop3(*c.in) ? c.f(Field::VDST) : 106;
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
        if (!no_sdwa_dpp(c)) return false;
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

    // -- conversions --
    float_unary(t, "v_cvt_f32_u32", "float(asuint(X))");
    float_unary(t, "v_cvt_f32_i32", "float(asint(X))");
    for (int n = 0; n < 4; ++n) {
      t["v_cvt_f32_ubyte" + std::to_string(n)] = [n](Ctx& c) {
        if (!no_sdwa_dpp(c)) return false;
        c.vwrite(vop_dst(c), fresult(c, "float((" + vsrc(c, 0) + " >> " + std::to_string(8 * n) + "u) & 0xFFu)"));
        return true;
      };
    }
    int_op(t, "v_cvt_u32_f32", "am_cvt_u32(asfloat(A))");
    int_op(t, "v_cvt_i32_f32", "asuint(am_cvt_i32(asfloat(A)))");
    t["v_cvt_f32_f16"] = [](Ctx& c) {
      if (!no_sdwa_dpp(c)) return false;
      c.vwrite(vop_dst(c), fresult(c, "f16tof32(" + vsrc(c, 0, true) + ")"));
      return true;
    };
    int_op(t, "v_cvt_f16_f32", "f32tof16(asfloat(A))");
    // Round toward zero; f32tof16 rounds to nearest: the low bit may differ (noted in the plan).
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
    int_op(t, "v_mul_u32_u24", "((A & 0xFFFFFFu) * (B & 0xFFFFFFu))");
    int_op(t, "v_mad_u32_u24", "((A & 0xFFFFFFu) * (B & 0xFFFFFFu) + C)");
    int_op(t, "v_bfe_u32", "am_bfe_u32(A, B, C)");
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
    t["v_nop"] = [](Ctx&) { return true; };
    t["v_lshlrev_b64"] = [](Ctx& c) {
      if (!no_sdwa_dpp(c)) return false;
      const uint32_t d = vop_dst(c);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint64_t r = " + c.src64(c.f(Field::SRC1)) + " << (" + vsrc(c, 0) + " & 63u); v[" + num(d) +
             "] = uint(r); v[" + num(d + 1) + "] = uint(r >> 32); }");
      return true;
    };
    // VOP2: VCC picks src1; VOP3: the mask in src2.
    t["v_cndmask_b32"] = [](Ctx& c) {
      if (!no_sdwa_dpp(c)) return false;
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
      if (!no_sdwa_dpp(c) || !is_vop3(*c.in)) return c.fail("v_add_co_u32 form");
      const uint32_t sd = c.f(Field::SDST);
      c.line("{ uint a = " + vsrc(c, 0) + ", b = " + vsrc(c, 1) + "; uint r = a + b; bool cy = r < a;");
      c.line("  uint4 m = WaveActiveBallot(cy && AM_ACT != 0u);");
      c.line("  if (AM_ACT) " + c.vdst(vop_dst(c)) + " = r;");
      if (sd != 125) c.line("  s[" + num(sd) + "] = m.x;" + (c.wave64 ? " s[" + num(sd + 1) + "] = m.y;" : std::string()));
      c.line("}");
      return true;
    };
    t["v_add_co_ci_u32"] = [](Ctx& c) {
      if (!no_sdwa_dpp(c)) return false;
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
    for (const Cmp& x : fcmp) compare(t, std::string(x.name) + "_f32", x.cond, true);
    const Cmp icmp[] = {{"f", "false"}, {"lt", "A < B"}, {"eq", "A == B"}, {"le", "A <= B"}, {"gt", "A > B"},
                        {"ne", "A != B"}, {"ge", "A >= B"}, {"t", "true"}};
    for (const Cmp& x : icmp) {
      compare(t, std::string(x.name) + "_u32", x.cond, false);
      compare(t, std::string(x.name) + "_i32", fill(x.cond, "asint(A)", "asint(B)"), false);
      compare(t, std::string(x.name) + "_u64", x.cond, false, true);
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
    };
    for (const S1& x : sop1) {
      t[x.name] = [x](Ctx& c) {
        c.line("{ uint a = " + c.src(c.f(Field::SSRC0)) + "; uint r = " + x.expr + ";" +
               (std::string(x.scc).empty() ? "" : " scc = " + std::string(x.scc) + ";") + " " + c.sdst(c.f(Field::SDST)) +
               " = r; }");
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
    // -- buffer and global memory (raw dwords) --
    for (int n : {1, 2, 3, 4}) {
      const std::string sfx = n == 1 ? "" : "x" + std::to_string(n);
      for (const bool store : {false, true}) {
        t[(store ? "buffer_store_dword" : "buffer_load_dword") + sfx] = [n, store](Ctx& c) {
          if (c.f(Field::LDS) || c.f(Field::TFE)) return c.fail("buffer access with lds/tfe");
          const uint32_t r = c.f(Field::SRSRC) * 4, va = c.f(Field::VADDR), vd = c.f(Field::VDATA);
          const bool idx = c.f(Field::IDXEN), off = c.f(Field::OFFEN);
          c.touch_v(va + (idx && off ? 1 : 0));
          c.touch_v(vd + n - 1);
          const std::string index = idx ? "v[" + num(va) + "]" : "0u";
          const std::string offset = off ? "v[" + num(va + (idx ? 1 : 0)) + "]" : "0u";
          c.line("if (AM_ACT) { uint4 rs = uint4(s[" + num(r) + "], s[" + num(r + 1) + "], s[" + num(r + 2) + "], s[" + num(r + 3) + "]);");
          c.line("  uint64_t p; uint lim; am_buffer_address(rs, " + index + ", " + offset + " + " + num(c.f(Field::OFFSET)) + "u, " +
                 c.src(c.f(Field::SOFFSET)) + ", p, lim);");
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
    // -- LDS --
    t["ds_read_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      c.touch_v(c.f(Field::ADDR));
      c.vwrite(c.f(Field::VDST), "am_lds[(v[" + num(c.f(Field::ADDR)) + "] + " + num(c.f(Field::OFFSET0) | c.f(Field::OFFSET1) << 8) + "u) >> 2]");
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
    t["ds_read2_b32"] = [](Ctx& c) {
      c.uses_lds = true;
      const uint32_t a = c.f(Field::ADDR), d = c.f(Field::VDST);
      c.touch_v(a);
      c.touch_v(d + 1);
      c.line("if (AM_ACT) { uint b = v[" + num(a) + "] >> 2; v[" + num(d) + "] = am_lds[b + " + num(c.f(Field::OFFSET0)) +
             "u]; v[" + num(d + 1) + "] = am_lds[b + " + num(c.f(Field::OFFSET1)) + "u]; }");
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
uint am_wqm(uint a) {
  uint r = 0u;
  [unroll] for (uint q = 0u; q < 8u; ++q) if ((a >> (4u * q)) & 0xFu) r |= 0xFu << (4u * q);
  return r;
}
// v_perm_b32: each selector byte picks a byte of {s0, s1} or a constant.
uint am_perm(uint s0, uint s1, uint sel) {
  uint64_t x = (uint64_t(s0) << 32) | s1;
  uint r = 0u;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    uint k = (sel >> (8u * i)) & 0xFFu, b;
    if (k <= 7u) b = uint(x >> (8u * k)) & 0xFFu;
    else if (k <= 11u) {  // the sign of byte 1, 3, 5 or 7, spread
      uint src = 2u * (k - 8u) + 1u;
      b = (uint(x >> (8u * src)) & 0x80u) ? 0xFFu : 0u;
    } else if (k == 12u) b = 0u;
    else b = 0xFFu;
    r |= b << (8u * i);
  }
  return r;
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
cbuffer am_args : register(b0) { uint4 am_user[8]; uint2 am_base; uint2 am_pad; };
uint am_offset(uint64_t a) { return uint(a - am_u64(am_base.x, am_base.y)); }
uint am_load(uint64_t a) { return am_memory.Load(am_offset(a)); }
void am_store(uint64_t a, uint x) { am_memory.Store(am_offset(a), x); }
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
)";

}  // namespace

Translation translate(const TranslateInput& in, Binding binding) {
  Translation tr;
  tr.wave_size = in.wave32 ? 32 : 64;
  if (in.stage != HwStage::Cs) {
    tr.failure = std::string("stage ") + hw_stage_name(in.stage) + " (compute only so far)";
    tr.unsupported[tr.failure] = 1;
    return tr;
  }
  if (binding != Binding::Test) {
    tr.failure = "the runtime binding model comes with M4";
    tr.unsupported[tr.failure] = 1;
    return tr;
  }
  const rd::ProgramScan scan = rd::scan_program(in.code);
  if (scan.errors) {
    tr.failure = "decoding: " + scan.first_error_reason;
    tr.unsupported[tr.failure] = 1;
    return tr;
  }
  if (scan.indirect) {
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
  c.wave64 = !in.wave32;
  std::string body;
  const auto& ops = table();
  bool open = false;  // the current block can fall through into the next
  for (size_t k = 0; k < scan.starts.size(); ++k) {
    const uint32_t at = scan.starts[k];
    rd::Instruction insn;
    rd::decode(in.code, at, insn);
    const uint32_t next = at + insn.dwords;
    if (starts.count(at)) {
      if (open) body += "      pc = " + num(at) + "u; break;\n";
      body += "    case " + num(at) + "u:\n";
    }
    open = true;
    c.in = &insn;
    c.pc = at;
    c.next = next;
    c.out.clear();
    const std::string name = insn.info->name;
    const uint16_t flags = insn.info->flags;
    bool ok = true;
    if (flags & rd::kTerminator) {
      c.line("AM_END(); return;");
      open = false;
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
        const std::string to = num(static_cast<uint64_t>(target)) + "u";
        if (name == "s_branch") {
          c.line("pc = " + to + "; break;");
          open = false;
        } else {
          c.line("if (" + cond + ") { pc = " + to + "; break; }");
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
    body += c.out;
  }
  if (open) body += "      return;\n";
  if (!tr.unsupported.empty()) return tr;

  const uint32_t nv = std::max<uint32_t>({c.max_vgpr, in.vgprs ? in.vgprs : 1u, 3u});
  tr.vgpr_count = nv;
  const uint32_t user = (in.cs_rsrc2 >> 1) & 31;
  const uint32_t tid_comps = (in.cs_rsrc2 >> 11) & 3;
  std::string h;
  h += "// Translated by Project Americas' shader translator, version " + num(kTranslatorVersion) + ".\n";
  h += kHelpers;
  h += kTestBinding;
  h += kBufferHelpers;
  if (c.uses_lds || in.lds_bytes) h += "groupshared uint am_lds[" + num(std::max<uint32_t>(in.lds_bytes / 4, 1)) + "];\n";
  const uint32_t tx = std::max<uint32_t>(in.cs_threads[0], 1), ty = std::max<uint32_t>(in.cs_threads[1], 1),
                 tz = std::max<uint32_t>(in.cs_threads[2], 1);
  h += "\n[numthreads(" + num(tx) + ", " + num(ty) + ", " + num(tz) + ")]\n[WaveSize(" + num(tr.wave_size) + ")]\n";
  h += "void main(uint3 am_tid : SV_GroupThreadID, uint3 am_gid : SV_GroupID, uint am_flat : SV_GroupIndex) {\n";
  h += "  uint s[128];\n  [unroll] for (uint i = 0u; i < 128u; ++i) s[i] = 0u;\n";
  h += "  uint v[" + num(nv) + "];\n  [unroll] for (uint j = 0u; j < " + num(nv) + "u; ++j) v[j] = 0u;\n";
  h += "  uint am_null = 0u;\n  bool scc = false;\n  uint am_lane = WaveGetLaneIndex();\n";
  for (uint32_t i = 0; i < user; ++i) h += "  s[" + num(i) + "] = am_user[" + num(i / 4) + "][" + num(i % 4) + "];\n";
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
    h += "#define am_lanes_below_hi() (am_lane < 32u ? 0u : (1u << (am_lane - 32u)) - 1u)\n";
  } else {
    h += "#define AM_ACT ((s[126] >> am_lane) & 1u)\n";
    h += "#define am_exec_zero() (s[126] == 0u)\n#define am_vcc_zero() (s[106] == 0u)\n";
    h += "#define am_lane_bit(lo, hi) ((((lo) >> am_lane) & 1u) != 0u)\n";
    h += "#define am_first_active() (s[126] != 0u ? firstbitlow(s[126]) : 0u)\n";
    h += "#define am_lanes_below_lo() ((1u << am_lane) - 1u)\n#define am_lanes_below_hi() 0u\n";
  }
  // At s_endpgm the tests' binding dumps the registers.
  h += "#define AM_END() { [unroll] for (uint d = 0u; d < " + num(nv) + "u; ++d) am_dump.Store(" + num(kDumpLanes) +
       "u + ((am_gid.x * " + num(tx * ty * tz) + "u + am_flat) * " + num(nv) + "u + d) * 4u, v[d]); " +
       "if (am_flat == 0u && all(am_gid == 0u)) { [unroll] for (uint q = 0u; q < 128u; ++q) am_dump.Store(" +
       num(kDumpSgprs) + "u + q * 4u, s[q]); am_dump.Store(" + num(kDumpSgprs) + "u + 512u, scc ? 1u : 0u); } }\n";
  h += "  uint pc = 0u;\n  [loop] for (;;) {\n    [forcecase] switch (pc) {\n";
  h += body;
  h += "    default:\n      return;\n    }\n  }\n}\n";
  tr.hlsl = std::move(h);
  tr.ok = true;
  return tr;
}

}  // namespace am::shader
