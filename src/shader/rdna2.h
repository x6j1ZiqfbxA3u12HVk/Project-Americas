#pragma once

// An RDNA 2 (GFX10.3) instruction decoder: the console GPU's machine code. The instruction and
// field tables are generated from AMD's machine-readable ISA specification
// (shader/rdna2_tables.inl); the rules for an instruction's length are the ISA's:
//
// * a 32-bit literal follows when a source operand field holds 255 (one literal per
//   instruction), and always for s_setreg_imm32_b32 and the v_fma*k forms;
// * VOP1 / VOP2 / VOPC take an extra dword for SDWA (src0 249), DPP16 (250) or DPP8 (233, 234);
// * MIMG takes 1 to 3 extra dwords of addresses in its NSA form.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace am::shader::rdna2 {

enum class Encoding : uint8_t {
  Sop1, Sopc, Sopp, Sopk, Sop2, Smem, Vop1, Vopc, Vop2, Vintrp, Vop3, Vop3p, Ds, Mubuf, Mtbuf, Mimg, Exp, Flat,
  FlatGlobal, FlatScratch, Count
};
const char* encoding_name(Encoding e);

enum class Field : uint8_t {
#define AM_FIELD(x) x,
#define AM_RDNA2_FIELDS
#include "shader/rdna2_tables.inl"
#undef AM_RDNA2_FIELDS
#undef AM_FIELD
  Count
};

enum class OperandType : uint8_t {
#define AM_TYPE(x) x,
#define AM_RDNA2_TYPES
#include "shader/rdna2_tables.inl"
#undef AM_RDNA2_TYPES
#undef AM_TYPE
  Count
};

// The specification's encodings, as layouts of fields (literal, SDWA, DPP and NSA forms too).
enum class Layout : uint8_t {
#define AM_LAYOUT_NAME(x) x,
#define AM_RDNA2_LAYOUT_NAMES
#include "shader/rdna2_tables.inl"
#undef AM_RDNA2_LAYOUT_NAMES
#undef AM_LAYOUT_NAME
  Count
};

// Instruction flags.
constexpr uint16_t kBranch = 1, kConditional = 2, kIndirect = 4, kTerminator = 8, kImmediate = 16;
constexpr uint16_t kLiteral = 32;  // its only form has a literal
constexpr uint16_t kSdst = 64;     // VOP3 with a scalar destination (VOP3b)
constexpr uint16_t kLegacy = 128;  // from RDNA 1: PC RDNA 2 dropped it, the console GPU runs it
// Operand flags.
constexpr uint8_t kIn = 1, kOut = 2, kImplicit = 4;

struct OperandInfo {
  Field field;
  OperandType type;
  uint16_t bits;
  uint8_t flags;
};

constexpr int kMaxOperands = 8;
struct InstructionInfo {
  Encoding encoding;
  uint16_t opcode;
  const char* name;
  uint16_t flags;
  uint8_t operand_count;
  OperandInfo operands[kMaxOperands];
};

// The table entry for an opcode of an encoding; nullptr if the specification has none.
const InstructionInfo* find(Encoding encoding, uint32_t opcode);

enum class Form : uint8_t { Plain, Sdwa, Dpp16, Dpp8 };

struct Instruction {
  const InstructionInfo* info = nullptr;  // nullptr for an unknown opcode
  Encoding encoding = Encoding::Count;
  Layout layout = Layout::Count;
  Form form = Form::Plain;
  uint16_t opcode = 0;
  uint8_t dwords = 0;
  bool has_literal = false;
  uint32_t words[5] = {};

  // A field of the instruction, by its layout; 0 when the layout has no such field.
  uint32_t field(Field f) const;
  bool has_field(Field f) const;
  uint32_t literal() const { return has_literal ? words[dwords - 1] : 0; }
};

enum class DecodeStatus : uint8_t { Ok, Truncated, UnknownEncoding, UnknownOpcode };
const char* decode_status_name(DecodeStatus s);

// Decodes the instruction at code[index]. For an unknown opcode `out` still says how many
// dwords the encoding takes, so a caller can go on.
DecodeStatus decode(std::span<const uint32_t> code, size_t index, Instruction& out);

// The instruction's name and operands' raw fields, for listings: "v_add_f32 [VDST=1 ...]".
std::string describe(const Instruction& insn);

// A program decoded by control flow: from its entry (dword 0), following branches, ending a
// path at s_endpgm, s_branch and s_setpc. Code never reached is data (the compiler appends
// some after s_endpgm) and is not decoded.
struct ProgramScan {
  std::vector<uint32_t> starts;   // dword index of each reachable instruction, ascending
  std::vector<uint32_t> targets;  // branch and call targets, ascending
  bool ends = false;              // a reachable s_endpgm
  bool indirect = false;          // a reachable s_setpc / s_swappc: control we cannot follow
  size_t errors = 0;              // reachable dwords that do not decode, bad branch targets
  size_t first_error = static_cast<size_t>(-1);
  std::string first_error_reason;
};
ProgramScan scan_program(std::span<const uint32_t> code);

}  // namespace am::shader::rdna2
