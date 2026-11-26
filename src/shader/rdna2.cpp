#include "shader/rdna2.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace am::shader::rdna2 {
namespace {

#define AM_OP(field, type, bits, flags) OperandInfo{Field::field, OperandType::type, bits, flags}
#define AM_INSN(enc, opcode, name, flags, count, ...) InstructionInfo{Encoding::enc, opcode, name, flags, count, {__VA_ARGS__}},
#define AM_RDNA2_INSTRUCTIONS
const InstructionInfo kInstructions[] = {
#include "shader/rdna2_tables.inl"
};
#undef AM_RDNA2_INSTRUCTIONS
#undef AM_INSN
#undef AM_OP

struct FieldBits {
  uint8_t first = 0;
  uint8_t bits = 0;  // 0: not in this layout
};

struct Tables {
  std::array<std::array<FieldBits, static_cast<size_t>(Field::Count)>, static_cast<size_t>(Layout::Count)> layouts{};
  std::array<std::vector<const InstructionInfo*>, static_cast<size_t>(Encoding::Count)> by_opcode;
};

const Tables& tables() {
  static const Tables t = [] {
    Tables x;
#define AM_LAYOUT(layout, field, first, bits) \
  x.layouts[static_cast<size_t>(Layout::layout)][static_cast<size_t>(Field::field)] = FieldBits{first, bits};
#define AM_RDNA2_LAYOUTS
#include "shader/rdna2_tables.inl"
#undef AM_RDNA2_LAYOUTS
#undef AM_LAYOUT
    for (const InstructionInfo& i : kInstructions) {
      auto& v = x.by_opcode[static_cast<size_t>(i.encoding)];
      if (v.size() <= i.opcode) v.resize(i.opcode + 1u, nullptr);
      v[i.opcode] = &i;
    }
    return x;
  }();
  return t;
}

uint32_t extract(const uint32_t* words, int dwords, FieldBits fb) {
  if (!fb.bits) return 0;
  const int word = fb.first / 32, shift = fb.first % 32;
  uint64_t v = words[word];
  if (word + 1 < dwords) v |= static_cast<uint64_t>(words[word + 1]) << 32;
  v >>= shift;
  return static_cast<uint32_t>(fb.bits >= 32 ? v : v & ((uint64_t{1} << fb.bits) - 1));
}

FieldBits bits_of(Layout layout, Field field) {
  return tables().layouts[static_cast<size_t>(layout)][static_cast<size_t>(field)];
}

// The encoding from the first dword's fixed bits.
Encoding classify(uint32_t w) {
  if ((w >> 31) == 0) {
    const uint32_t top7 = w >> 25;
    if (top7 == 0x3F) return Encoding::Vop1;
    if (top7 == 0x3E) return Encoding::Vopc;
    return Encoding::Vop2;
  }
  switch (w >> 23) {
    case 0x17D: return Encoding::Sop1;
    case 0x17E: return Encoding::Sopc;
    case 0x17F: return Encoding::Sopp;
    default: break;
  }
  if ((w >> 28) == 0xB) return Encoding::Sopk;
  if ((w >> 30) == 0x2) return Encoding::Sop2;
  switch (w >> 26) {
    case 0x32: return Encoding::Vintrp;
    case 0x33: return Encoding::Vop3p;
    case 0x35: return Encoding::Vop3;
    case 0x36: return Encoding::Ds;
    case 0x37:
      switch ((w >> 14) & 3) {
        case 0: return Encoding::Flat;
        case 1: return Encoding::FlatScratch;
        case 2: return Encoding::FlatGlobal;
        default: return Encoding::Count;
      }
    case 0x38: return Encoding::Mubuf;
    case 0x3A: return Encoding::Mtbuf;
    case 0x3C: return Encoding::Mimg;
    case 0x3D: return Encoding::Smem;
    case 0x3E: return Encoding::Exp;
    default: return Encoding::Count;
  }
}

Layout base_layout(Encoding e) {
  switch (e) {
    case Encoding::Sop1: return Layout::ENC_SOP1;
    case Encoding::Sopc: return Layout::ENC_SOPC;
    case Encoding::Sopp: return Layout::ENC_SOPP;
    case Encoding::Sopk: return Layout::ENC_SOPK;
    case Encoding::Sop2: return Layout::ENC_SOP2;
    case Encoding::Smem: return Layout::ENC_SMEM;
    case Encoding::Vop1: return Layout::ENC_VOP1;
    case Encoding::Vopc: return Layout::ENC_VOPC;
    case Encoding::Vop2: return Layout::ENC_VOP2;
    case Encoding::Vintrp: return Layout::ENC_VINTRP;
    case Encoding::Vop3: return Layout::ENC_VOP3;
    case Encoding::Vop3p: return Layout::ENC_VOP3P;
    case Encoding::Ds: return Layout::ENC_DS;
    case Encoding::Mubuf: return Layout::ENC_MUBUF;
    case Encoding::Mtbuf: return Layout::ENC_MTBUF;
    case Encoding::Mimg: return Layout::ENC_MIMG;
    case Encoding::Exp: return Layout::ENC_EXP;
    case Encoding::Flat: return Layout::ENC_FLAT;
    case Encoding::FlatGlobal: return Layout::ENC_FLAT_GLBL;
    case Encoding::FlatScratch: return Layout::ENC_FLAT_SCRATCH;
    case Encoding::Count: break;
  }
  return Layout::Count;
}

int base_dwords(Encoding e) {
  switch (e) {
    case Encoding::Sop1: case Encoding::Sopc: case Encoding::Sopp: case Encoding::Sopk: case Encoding::Sop2:
    case Encoding::Vop1: case Encoding::Vopc: case Encoding::Vop2: case Encoding::Vintrp:
      return 1;
    default:
      return 2;
  }
}

constexpr uint32_t kLiteralSource = 255;
constexpr uint32_t kSdwaSource = 249, kDpp16Source = 250, kDpp8Source = 233, kDpp8FiSource = 234;

bool literal_in_sources(Layout layout, const uint32_t* words, std::initializer_list<Field> fields) {
  for (Field f : fields) {
    // Every source field counts, used by the opcode or not: the compiler hides 32-bit markers in
    // v_nop this way (its unused src2 set to 255, the literal after it).
    if (extract(words, 2, bits_of(layout, f)) == kLiteralSource) return true;
  }
  return false;
}

}  // namespace

const char* encoding_name(Encoding e) {
  static const char* const names[] = {"SOP1", "SOPC", "SOPP", "SOPK", "SOP2", "SMEM", "VOP1", "VOPC", "VOP2", "VINTRP",
                                      "VOP3", "VOP3P", "DS", "MUBUF", "MTBUF", "MIMG", "EXP", "FLAT", "GLOBAL", "SCRATCH"};
  return e < Encoding::Count ? names[static_cast<int>(e)] : "?";
}

const char* decode_status_name(DecodeStatus s) {
  switch (s) {
    case DecodeStatus::Ok: return "ok";
    case DecodeStatus::Truncated: return "truncated";
    case DecodeStatus::UnknownEncoding: return "unknown encoding";
    case DecodeStatus::UnknownOpcode: return "unknown opcode";
  }
  return "?";
}

const InstructionInfo* find(Encoding encoding, uint32_t opcode) {
  if (encoding >= Encoding::Count) return nullptr;
  const auto& v = tables().by_opcode[static_cast<size_t>(encoding)];
  return opcode < v.size() ? v[opcode] : nullptr;
}

uint32_t Instruction::field(Field f) const { return extract(words, dwords, bits_of(layout, f)); }
bool Instruction::has_field(Field f) const { return bits_of(layout, f).bits != 0; }

DecodeStatus decode(std::span<const uint32_t> code, size_t index, Instruction& out) {
  out = Instruction{};
  if (index >= code.size()) return DecodeStatus::Truncated;
  const uint32_t w0 = code[index];
  out.words[0] = w0;
  out.encoding = classify(w0);
  if (out.encoding == Encoding::Count) {
    out.dwords = 1;
    return DecodeStatus::UnknownEncoding;
  }
  const Layout base = base_layout(out.encoding);
  int dwords = base_dwords(out.encoding);
  if (index + dwords > code.size()) {
    out.dwords = static_cast<uint8_t>(code.size() - index);
    return DecodeStatus::Truncated;
  }
  for (int k = 1; k < dwords; ++k) out.words[k] = code[index + k];
  out.opcode = static_cast<uint16_t>(extract(out.words, dwords, bits_of(base, Field::OP)));
  // MIMG's opcode has an eighth bit at bit 0 (the specification's layout leaves it out): the
  // ray-tracing instructions are 230 and 231.
  if (out.encoding == Encoding::Mimg) out.opcode = static_cast<uint16_t>(out.opcode | (w0 & 1) << 7);
  // MUBUF's eighth bit is bit 25: the D16 buffer loads and stores are 128 and up.
  if (out.encoding == Encoding::Mubuf) out.opcode = static_cast<uint16_t>(out.opcode | ((w0 >> 25) & 1) << 7);
  out.info = find(out.encoding, out.opcode);
  const InstructionInfo* info = out.info;
  const bool always_literal = info && (info->flags & kLiteral);
  Layout layout = base;
  bool literal = always_literal;
  int extra = 0;

  switch (out.encoding) {
    case Encoding::Sop1:
      literal = literal || extract(out.words, 1, bits_of(base, Field::SSRC0)) == kLiteralSource;
      if (literal) layout = Layout::SOP1_INST_LITERAL;
      break;
    case Encoding::Sop2:
    case Encoding::Sopc:
      literal = literal || extract(out.words, 1, bits_of(base, Field::SSRC0)) == kLiteralSource ||
                extract(out.words, 1, bits_of(base, Field::SSRC1)) == kLiteralSource;
      if (literal) layout = out.encoding == Encoding::Sop2 ? Layout::SOP2_INST_LITERAL : Layout::SOPC_INST_LITERAL;
      break;
    case Encoding::Sopk:
      if (literal) layout = Layout::SOPK_INST_LITERAL;
      break;
    case Encoding::Vop1:
    case Encoding::Vop2:
    case Encoding::Vopc: {
      const uint32_t src0 = w0 & 0x1FF;
      const bool vop1 = out.encoding == Encoding::Vop1, vop2 = out.encoding == Encoding::Vop2;
      // The VOP2 carry instructions write VCC: their SDWA form has a scalar destination.
      const bool carry = vop2 && info && std::string_view(info->name).ends_with("_co_ci_u32");
      if (src0 == kSdwaSource) {
        out.form = Form::Sdwa;
        extra = 1;
        layout = vop1 ? Layout::VOP1_VOP_SDWA
                 : vop2 ? (carry ? Layout::VOP2_VOP_SDWA_SDST_ENC : Layout::VOP2_VOP_SDWA)
                        : Layout::VOPC_VOP_SDWA_SDST_ENC;
      } else if (src0 == kDpp16Source && out.encoding != Encoding::Vopc) {
        out.form = Form::Dpp16;
        extra = 1;
        layout = vop1 ? Layout::VOP1_VOP_DPP16 : Layout::VOP2_VOP_DPP16;
      } else if ((src0 == kDpp8Source || src0 == kDpp8FiSource) && out.encoding != Encoding::Vopc) {
        out.form = Form::Dpp8;
        extra = 1;
        layout = vop1 ? Layout::VOP1_VOP_DPP8 : Layout::VOP2_VOP_DPP8;
      } else if (src0 == kLiteralSource) {
        literal = true;
      }
      if (literal && out.form == Form::Plain) {
        layout = vop1 ? Layout::VOP1_INST_LITERAL : vop2 ? Layout::VOP2_INST_LITERAL : Layout::VOPC_INST_LITERAL;
      }
      break;
    }
    case Encoding::Vop3: {
      const bool sdst = info && (info->flags & kSdst);
      if (sdst) layout = Layout::VOP3_SDST_ENC;
      literal = literal_in_sources(layout, out.words, {Field::SRC0, Field::SRC1, Field::SRC2});
      if (literal) layout = sdst ? Layout::VOP3_SDST_ENC_INST_LITERAL : Layout::VOP3_INST_LITERAL;
      break;
    }
    case Encoding::Vop3p:
      literal = literal_in_sources(base, out.words, {Field::SRC0, Field::SRC1, Field::SRC2});
      if (literal) layout = Layout::VOP3P_INST_LITERAL;
      break;
    case Encoding::Mimg: {
      const uint32_t nsa = (w0 >> 1) & 3;
      extra = static_cast<int>(nsa);
      if (nsa) layout = nsa == 1 ? Layout::MIMG_NSA1 : nsa == 2 ? Layout::MIMG_NSA2 : Layout::MIMG_NSA3;
      break;
    }
    default:
      break;
  }
  dwords += extra + (literal ? 1 : 0);
  if (index + dwords > code.size()) {
    out.dwords = static_cast<uint8_t>(code.size() - index);
    return DecodeStatus::Truncated;
  }
  for (int k = base_dwords(out.encoding); k < dwords; ++k) out.words[k] = code[index + k];
  out.dwords = static_cast<uint8_t>(dwords);
  out.layout = layout;
  out.has_literal = literal;
  return info ? DecodeStatus::Ok : DecodeStatus::UnknownOpcode;
}

std::string describe(const Instruction& insn) {
  std::string s = insn.info ? insn.info->name : std::string(encoding_name(insn.encoding)) + " op " + std::to_string(insn.opcode);
  if (insn.form == Form::Sdwa) s += " (sdwa)";
  if (insn.form == Form::Dpp16) s += " (dpp)";
  if (insn.form == Form::Dpp8) s += " (dpp8)";
  if (insn.info) {
    for (int k = 0; k < insn.info->operand_count; ++k) {
      const OperandInfo& o = insn.info->operands[k];
      if (o.flags & kImplicit) continue;
      s += k ? ", " : " ";
      s += std::to_string(insn.field(o.field));
    }
  }
  if (insn.has_literal) {
    char buf[16];
    std::snprintf(buf, sizeof buf, " lit 0x%08x", insn.literal());
    s += buf;
  }
  return s;
}

namespace {

// The constant target (dword) of the far-jump idiom ending in the s_setpc_b64 `jump`, whose
// three instructions before it on this path start at `recent` (oldest first); -1 when it is not
// one. Our code starts at address 0, so s_getpc_b64 gives the next instruction's byte offset.
int64_t far_target(std::span<const uint32_t> code, const size_t* recent, const Instruction& jump) {
  Instruction get, add, addc;
  if (decode(code, recent[0], get) != DecodeStatus::Ok || decode(code, recent[1], add) != DecodeStatus::Ok ||
      decode(code, recent[2], addc) != DecodeStatus::Ok) {
    return -1;
  }
  const uint32_t d = jump.field(Field::SSRC0);
  if (std::string_view(get.info->name) != "s_getpc_b64" || get.field(Field::SDST) != d) return -1;
  // Forward: s_add_u32 / s_addc_u32; backward: s_sub_u32 / s_subb_u32.
  const std::string_view an = add.info->name, cn = addc.info->name;
  const bool sub = an == "s_sub_u32";
  if ((an != "s_add_u32" && !sub) || add.field(Field::SDST) != d || add.field(Field::SSRC0) != d) return -1;
  if (cn != (sub ? "s_subb_u32" : "s_addc_u32") || addc.field(Field::SDST) != d + 1 || addc.field(Field::SSRC0) != d + 1) return -1;
  uint32_t k = 0;
  const uint32_t ks = add.field(Field::SSRC1);
  if (ks == 255) k = add.literal();
  else if (ks >= 128 && ks <= 192) k = ks - 128;
  else if (ks >= 193 && ks <= 208) k = static_cast<uint32_t>(-static_cast<int32_t>(ks - 192));
  else return -1;
  const uint32_t hs = addc.field(Field::SSRC1);
  if (hs != 128 && hs != 193) return -1;  // 0 or -1
  const uint64_t h = hs == 193 ? 0xFFFFFFFFull : 0;
  const uint64_t base = (recent[0] + get.dwords) * 4ull;
  const uint64_t base_lo = base & 0xFFFFFFFFull, base_hi = base >> 32;
  uint64_t lo, hi;
  if (sub) {
    const bool borrow = k > base_lo;
    lo = (base_lo - k) & 0xFFFFFFFFull;
    hi = (base_hi - h - (borrow ? 1 : 0)) & 0xFFFFFFFFull;
  } else {
    lo = base_lo + k;
    hi = (base_hi + h + (lo >> 32)) & 0xFFFFFFFFull;
    lo &= 0xFFFFFFFFull;
  }
  const uint64_t target = hi << 32 | lo;
  if (target % 4 || target / 4 >= code.size()) return -1;
  return static_cast<int64_t>(target / 4);
}

}  // namespace

int64_t far_jump_target(const ProgramScan& scan, uint32_t at) {
  const auto it = std::lower_bound(scan.far_jumps.begin(), scan.far_jumps.end(), std::pair<uint32_t, uint32_t>{at, 0});
  return it != scan.far_jumps.end() && it->first == at ? static_cast<int64_t>(it->second) : -1;
}

ProgramScan scan_program(std::span<const uint32_t> code) {
  ProgramScan r;
  std::vector<uint8_t> state(code.size(), 0);  // 1: an instruction starts here, 2: inside one
  std::vector<uint32_t> work{0};
  const auto error = [&](size_t at, const char* why) {
    ++r.errors;
    if (at < r.first_error) {
      r.first_error = at;
      r.first_error_reason = why;
    }
  };
  const auto branch = [&](size_t next, uint32_t simm16) -> bool {
    const int64_t target = static_cast<int64_t>(next) + static_cast<int16_t>(simm16);
    if (target < 0 || static_cast<uint64_t>(target) >= code.size()) return false;
    r.targets.push_back(static_cast<uint32_t>(target));
    work.push_back(static_cast<uint32_t>(target));
    return true;
  };
  while (!work.empty()) {
    size_t pc = work.back();
    work.pop_back();
    size_t recent[3] = {}, seen = 0;  // the path's last three instructions (for far jumps)
    while (pc < code.size()) {
      if (state[pc] == 1) break;  // decoded already
      if (state[pc] == 2) {
        error(pc, "branch into an instruction");
        break;
      }
      Instruction insn;
      const DecodeStatus st = decode(code, pc, insn);
      if (st != DecodeStatus::Ok) {
        error(pc, decode_status_name(st));
        break;
      }
      state[pc] = 1;
      for (size_t k = 1; k < insn.dwords; ++k) {
        if (state[pc + k] == 1) error(pc + k, "instructions overlap");
        state[pc + k] = 2;
      }
      r.starts.push_back(static_cast<uint32_t>(pc));
      const size_t next = pc + insn.dwords;
      const uint16_t flags = insn.info->flags;
      // A far jump (the three instructions before it on this path make its target): followed.
      if (insn.encoding == Encoding::Sop1 && insn.opcode == 32 && seen >= 3) {
        if (const int64_t target = far_target(code, recent, insn); target >= 0) {
          r.far_jumps.emplace_back(static_cast<uint32_t>(pc), static_cast<uint32_t>(target));
          r.targets.push_back(static_cast<uint32_t>(target));
          work.push_back(static_cast<uint32_t>(target));
          break;
        }
      }
      recent[0] = recent[1];
      recent[1] = recent[2];
      recent[2] = pc;
      if (seen < 3) ++seen;
      if (flags & kTerminator) {
        r.ends = true;
        break;
      }
      if (insn.encoding == Encoding::Sopp && insn.opcode == 31) {  // s_code_end: padding, never run
        error(pc, "s_code_end reached");
        break;
      }
      if (flags & kIndirect) {
        if (insn.encoding == Encoding::Sop1 && insn.opcode == 32) {  // s_setpc_b64: a jump we cannot follow
          r.indirect = true;
          r.setpc = true;
          break;
        }
        if (insn.encoding == Encoding::Sop1 && insn.opcode == 33) r.indirect = true;  // s_swappc_b64 returns
        pc = next;
        continue;
      }
      if (flags & kBranch) {
        if (!branch(next, insn.field(Field::SIMM16))) {
          error(pc, "branch target outside the code");
          break;
        }
        const bool call = insn.encoding == Encoding::Sopk;  // s_call_b64 returns
        if (!(flags & kConditional) && !call) break;
      }
      pc = next;
    }
    if (pc >= code.size()) error(code.size(), "runs off the end of the code");
  }
  std::sort(r.starts.begin(), r.starts.end());
  std::sort(r.targets.begin(), r.targets.end());
  std::sort(r.far_jumps.begin(), r.far_jumps.end());
  r.targets.erase(std::unique(r.targets.begin(), r.targets.end()), r.targets.end());
  return r;
}

}  // namespace am::shader::rdna2
