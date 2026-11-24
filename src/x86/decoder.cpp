#include "x86/decoder.h"

#include <array>

namespace am::x86 {
namespace {

// Operand layout of an opcode.
enum : uint16_t {
  kModrm = 1 << 0,
  kImm8 = 1 << 1,
  kImm16 = 1 << 2,
  kImmZ = 1 << 3,   // 16 or 32 bits by operand size (never 64)
  kImmV = 1 << 4,   // 16, 32 or 64 bits by operand size (MOV r, imm)
  kMoffs = 1 << 5,  // address-size offset (MOV AL/AX, moffs)
  kRel32 = 1 << 6,  // 32-bit branch displacement
  kBad = 1 << 7,    // invalid in 64-bit mode
};

constexpr std::array<uint16_t, 256> make_primary() {
  std::array<uint16_t, 256> t{};
  // 00-3F: eight ALU groups of Eb,Gb / Ev,Gv / Gb,Eb / Gv,Ev / AL,Ib / rAX,Iz
  for (int base = 0; base < 0x40; base += 8) {
    for (int k = 0; k < 4; ++k) t[base + k] = kModrm;
    t[base + 4] = kImm8;
    t[base + 5] = kImmZ;
  }
  for (int op : {0x06, 0x07, 0x0E, 0x16, 0x17, 0x1E, 0x1F, 0x27, 0x2F, 0x37, 0x3F}) t[op] = kBad;
  t[0x60] = t[0x61] = kBad;
  t[0x63] = kModrm;  // MOVSXD
  t[0x68] = kImmZ;
  t[0x69] = kModrm | kImmZ;
  t[0x6A] = kImm8;
  t[0x6B] = kModrm | kImm8;
  for (int op = 0x70; op <= 0x7F; ++op) t[op] = kImm8;  // Jcc rel8
  t[0x80] = kModrm | kImm8;
  t[0x81] = kModrm | kImmZ;
  t[0x82] = kBad;
  t[0x83] = kModrm | kImm8;
  for (int op = 0x84; op <= 0x8F; ++op) t[op] = kModrm;
  t[0x9A] = kBad;
  for (int op = 0xA0; op <= 0xA3; ++op) t[op] = kMoffs;
  t[0xA8] = kImm8;
  t[0xA9] = kImmZ;
  for (int op = 0xB0; op <= 0xB7; ++op) t[op] = kImm8;
  for (int op = 0xB8; op <= 0xBF; ++op) t[op] = kImmV;
  t[0xC0] = t[0xC1] = kModrm | kImm8;
  t[0xC2] = kImm16;
  t[0xC6] = kModrm | kImm8;
  t[0xC7] = kModrm | kImmZ;
  t[0xC8] = kImm16 | kImm8;  // ENTER
  t[0xCA] = kImm16;
  t[0xCD] = kImm8;
  t[0xCE] = kBad;
  for (int op = 0xD0; op <= 0xD3; ++op) t[op] = kModrm;
  t[0xD4] = t[0xD5] = t[0xD6] = kBad;
  for (int op = 0xD8; op <= 0xDF; ++op) t[op] = kModrm;  // x87
  for (int op = 0xE0; op <= 0xE7; ++op) t[op] = kImm8;   // LOOPcc, JRCXZ, IN, OUT
  t[0xE8] = t[0xE9] = kRel32;
  t[0xEA] = kBad;
  t[0xEB] = kImm8;
  t[0xF6] = t[0xF7] = kModrm;  // immediates depend on ModRM.reg
  t[0xFE] = t[0xFF] = kModrm;
  return t;
}

constexpr std::array<uint16_t, 256> make_0f() {
  std::array<uint16_t, 256> t{};
  for (auto& f : t) f = kModrm;
  for (int op : {0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x37, 0x77, 0xA0, 0xA1,
                 0xA2, 0xA8, 0xA9, 0xAA}) {
    t[op] = 0;
  }
  for (int op = 0xC8; op <= 0xCF; ++op) t[op] = 0;  // BSWAP
  for (int op = 0x80; op <= 0x8F; ++op) t[op] = kRel32;  // Jcc rel32
  for (int op : {0x70, 0x71, 0x72, 0x73, 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6}) t[op] = kModrm | kImm8;
  for (int op : {0x04, 0x0A, 0x0C, 0x24, 0x25, 0x26, 0x27, 0x36, 0x39, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x7A, 0x7B, 0xA6,
                 0xA7}) {
    t[op] = kBad;
  }
  return t;
}

constexpr auto kPrimary = make_primary();
constexpr auto k0F = make_0f();

int32_t read_signed(std::span<const uint8_t> code, size_t at, size_t size) {
  switch (size) {
    case 1: return static_cast<int8_t>(code[at]);
    case 2: return static_cast<int16_t>(code[at] | (code[at + 1] << 8));
    case 4:
      return static_cast<int32_t>(static_cast<uint32_t>(code[at]) | (static_cast<uint32_t>(code[at + 1]) << 8) |
                                  (static_cast<uint32_t>(code[at + 2]) << 16) |
                                  (static_cast<uint32_t>(code[at + 3]) << 24));
    default: return 0;
  }
}

}  // namespace

uint8_t Instruction::simd_prefix() const {
  if (map == Map::Vex || map == Map::Evex || map == Map::Xop) return vex_pp;
  if (rep == 0xF3) return 2;
  if (rep == 0xF2) return 3;
  return p66 ? 1 : 0;
}

DecodeStatus decode(std::span<const uint8_t> code, Instruction& out) {
  out = Instruction{};
  size_t i = 0;
  // Is there room for `n` more bytes?
  auto need = [&](size_t n) {
    if (i + n > 15) return DecodeStatus::Invalid;
    if (i + n > code.size()) return DecodeStatus::Truncated;
    return DecodeStatus::Ok;
  };
#define AM_NEED(n)                             \
  do {                                         \
    const DecodeStatus status_ = need(n);      \
    if (status_ != DecodeStatus::Ok) return status_; \
  } while (0)

  // Legacy prefixes and REX. A REX prefix only counts immediately before the opcode.
  for (;;) {
    AM_NEED(1);
    const uint8_t b = code[i];
    if (b >= 0x40 && b <= 0x4F) {
      out.rex = b;
      ++i;
      continue;
    }
    switch (b) {
      case 0x66: out.p66 = true; break;
      case 0x67: out.p67 = true; break;
      case 0xF0: out.lock = true; break;
      case 0xF2:
      case 0xF3: out.rep = b; break;
      case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65: out.segment = b; break;
      default: goto prefixes_done;
    }
    out.rex = 0;
    ++i;
  }
prefixes_done:

  bool modrm = false;
  size_t imm = 0;
  bool rel = false;  // the immediate is a branch displacement
  const uint8_t first = code[i];
  const bool vex_like = first == 0xC4 || first == 0xC5 || first == 0x62 ||
                        (first == 0x8F && i + 1 < code.size() && (code[i + 1] & 0x1F) >= 8);

  if (vex_like) {
    // VEX / EVEX / XOP: no 66, F2, F3, F0 or REX before them.
    if (out.p66 || out.rep || out.lock || out.rex) return DecodeStatus::Invalid;
    if (first == 0xC5) {
      AM_NEED(3);
      const uint8_t p = code[i + 1];
      out.map = Map::Vex;
      out.vex_map = 1;
      out.vex_l = (p >> 2) & 1;
      out.vex_pp = static_cast<uint8_t>(p & 3);
      i += 2;
    } else if (first == 0xC4) {
      AM_NEED(4);
      const uint8_t p0 = code[i + 1], p1 = code[i + 2];
      out.map = Map::Vex;
      out.vex_map = static_cast<uint8_t>(p0 & 0x1F);
      if (out.vex_map < 1 || out.vex_map > 3) return DecodeStatus::Invalid;
      out.vex_w = (p1 >> 7) & 1;
      out.vex_l = (p1 >> 2) & 1;
      out.vex_pp = static_cast<uint8_t>(p1 & 3);
      i += 3;
    } else if (first == 0x62) {
      AM_NEED(5);
      const uint8_t p0 = code[i + 1], p1 = code[i + 2], p2 = code[i + 3];
      if ((p1 & 0x04) == 0) return DecodeStatus::Invalid;
      out.map = Map::Evex;
      out.vex_map = static_cast<uint8_t>(p0 & 0x07);
      if (out.vex_map == 0 || out.vex_map == 4 || out.vex_map == 7) return DecodeStatus::Invalid;
      out.vex_w = (p1 >> 7) & 1;
      out.vex_l = ((p2 >> 5) & 3) != 0;
      out.vex_pp = static_cast<uint8_t>(p1 & 3);
      i += 4;
    } else {  // XOP
      AM_NEED(4);
      const uint8_t p0 = code[i + 1], p1 = code[i + 2];
      out.map = Map::Xop;
      out.vex_map = static_cast<uint8_t>(p0 & 0x1F);
      if (out.vex_map > 10) return DecodeStatus::Invalid;
      out.vex_w = (p1 >> 7) & 1;
      out.vex_l = (p1 >> 2) & 1;
      out.vex_pp = static_cast<uint8_t>(p1 & 3);
      i += 3;
    }
    out.opcode = code[i];
    out.opcode_offset = static_cast<uint8_t>(i);
    ++i;
    const uint8_t op = out.opcode;
    if (out.map == Map::Xop) {
      modrm = true;
      imm = out.vex_map == 8 ? 1 : out.vex_map == 10 ? 4 : 0;
    } else {
      modrm = !(out.map == Map::Vex && out.vex_map == 1 && op == 0x77);  // VZEROUPPER / VZEROALL
      const bool map1_imm = out.vex_map == 1 && ((op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 ||
                                                 op == 0xC5 || op == 0xC6);
      imm = (out.vex_map == 3 || map1_imm) ? 1 : 0;
    }
  } else if (first == 0x0F) {
    ++i;
    AM_NEED(1);
    const uint8_t second = code[i];
    if (second == 0x38 || second == 0x3A) {
      ++i;
      AM_NEED(1);
      out.map = second == 0x38 ? Map::Map0F38 : Map::Map0F3A;
      out.opcode = code[i];
      out.opcode_offset = static_cast<uint8_t>(i);
      ++i;
      modrm = true;
      imm = second == 0x3A ? 1 : 0;
    } else if (second == 0x0F) {
      out.map = Map::Amd3DNow;
      out.opcode = second;
      out.opcode_offset = static_cast<uint8_t>(i);
      ++i;
      modrm = true;
      imm = 1;  // the real opcode follows the operands
    } else {
      out.map = Map::Map0F;
      out.opcode = second;
      out.opcode_offset = static_cast<uint8_t>(i);
      ++i;
      const uint16_t f = k0F[second];
      if (f & kBad) return DecodeStatus::Invalid;
      modrm = (f & kModrm) != 0;
      if (f & kImm8) imm += 1;
      if (f & kRel32) {
        imm = 4;
        rel = true;
      }
      // 66 0F 78 /0 ib ib = EXTRQ, F2 0F 78 /r ib ib = INSERTQ (SSE4a); without them, VMREAD.
      if (second == 0x78 && (out.rep == 0xF2 || (out.p66 && out.rep == 0))) imm = 2;
    }
  } else {
    out.map = Map::Primary;
    out.opcode = first;
    out.opcode_offset = static_cast<uint8_t>(i);
    ++i;
    const uint16_t f = kPrimary[first];
    if (f & kBad) return DecodeStatus::Invalid;
    modrm = (f & kModrm) != 0;
    if (f & kImm8) imm += 1;
    if (f & kImm16) imm += 2;
    if (f & kImmZ) imm += (out.p66 && !out.rex_w()) ? 2 : 4;
    if (f & kImmV) imm += out.rex_w() ? 8 : out.p66 ? 2 : 4;
    if (f & kMoffs) imm += out.p67 ? 4 : 8;
    if (f & kRel32) {
      imm = 4;
      rel = true;
    }
    if ((first >= 0x70 && first <= 0x7F) || (first >= 0xE0 && first <= 0xE3) || first == 0xEB) rel = true;
  }

  if (modrm) {
    AM_NEED(1);
    out.has_modrm = true;
    out.modrm_offset = static_cast<uint8_t>(i);
    out.modrm = code[i];
    ++i;
    size_t disp = 0;
    const uint8_t mod = out.mod(), rm = out.rm();
    if (mod != 3) {
      if (rm == 4) {
        AM_NEED(1);
        out.has_sib = true;
        out.sib = code[i];
        ++i;
        if (mod == 0 && (out.sib & 7) == 5) disp = 4;  // no base, disp32
      }
      if (mod == 0 && rm == 5) {
        disp = 4;
        out.rip_relative = true;
      }
      if (mod == 1) disp = 1;
      if (mod == 2) disp = 4;
    }
    AM_NEED(disp);
    if (disp) {
      out.disp_offset = static_cast<uint8_t>(i);
      out.disp_size = static_cast<uint8_t>(disp);
      i += disp;
    }
  }

  // Groups whose immediate or validity depends on ModRM.reg.
  if (out.map == Map::Primary) {
    const uint8_t reg = out.reg();
    switch (out.opcode) {
      case 0xF6: if (reg <= 1) imm = 1; break;                                  // TEST Eb, Ib
      case 0xF7: if (reg <= 1) imm = (out.p66 && !out.rex_w()) ? 2 : 4; break;  // TEST Ev, Iz
      case 0xC6: if (reg != 0 && out.modrm != 0xF8) return DecodeStatus::Invalid; break;  // MOV / XABORT
      case 0xC7:
        if (out.modrm == 0xF8) rel = true;  // XBEGIN rel32
        else if (reg != 0) return DecodeStatus::Invalid;
        break;
      case 0x8F: if (reg != 0) return DecodeStatus::Invalid; break;  // POP Ev
      case 0xFE: if (reg > 1) return DecodeStatus::Invalid; break;
      case 0xFF: if (reg == 7) return DecodeStatus::Invalid; break;
      default: break;
    }
  }

  AM_NEED(imm);
  if (imm) {
    out.imm_offset = static_cast<uint8_t>(i);
    out.imm_size = static_cast<uint8_t>(imm);
    i += imm;
  }
  out.length = static_cast<uint8_t>(i);
#undef AM_NEED

  // Control flow.
  const uint8_t op = out.opcode;
  if (rel) out.branch_disp = read_signed(code, out.imm_offset, out.imm_size);
  if (out.map == Map::Primary) {
    if ((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) || (op == 0xC7 && rel)) out.flow = Flow::CondJump;
    else if (op == 0xEB || op == 0xE9) out.flow = Flow::Jump;
    else if (op == 0xE8) out.flow = Flow::Call;
    else if (op == 0xC2 || op == 0xC3 || op == 0xCA || op == 0xCB || op == 0xCF) out.flow = Flow::Return;
    else if (op == 0xCC || op == 0xF4 || (op == 0xCD && code[out.imm_offset] == 0x29)) out.flow = Flow::Stop;
    else if (op == 0xFF && (out.reg() == 2 || out.reg() == 3)) out.flow = Flow::IndirectCall;
    else if (op == 0xFF && (out.reg() == 4 || out.reg() == 5)) out.flow = Flow::IndirectJump;
  } else if (out.map == Map::Map0F) {
    if (op >= 0x80 && op <= 0x8F) out.flow = Flow::CondJump;
    else if (op == 0x0B || op == 0xB9 || op == 0xFF) out.flow = Flow::Stop;  // UD2, UD1, UD0
  }
  return DecodeStatus::Ok;
}

}  // namespace am::x86
