#pragma once

// x86-64 instruction decoding, as far as the loader needs it: instruction length, the layout
// of the operand bytes (ModRM, SIB, displacement, immediates), control flow, and the CPU
// feature an instruction requires. It does not produce mnemonics.

#include <cstdint>
#include <optional>
#include <span>

#include "host/cpu.h"

namespace am::x86 {

enum class Map : uint8_t {
  Primary,  // one-byte opcodes
  Map0F,
  Map0F38,
  Map0F3A,
  Vex,       // vex_map says which
  Evex,      // AVX-512 and later
  Xop,       // AMD XOP (pre-Zen)
  Amd3DNow,  // 0F 0F
};

enum class Flow : uint8_t {
  Next,          // continues with the next instruction
  Jump,          // direct jump
  CondJump,      // conditional direct jump, LOOP, JRCXZ, XBEGIN
  Call,          // direct call
  IndirectJump,  // JMP r/m
  IndirectCall,  // CALL r/m
  Return,        // RET, IRET
  Stop,          // INT3, UD2, HLT, INT 29h (fast fail): nothing follows
};

struct Instruction {
  uint8_t length = 0;
  Map map = Map::Primary;
  uint8_t vex_map = 0;  // VEX / EVEX / XOP opcode map (1 = 0F, 2 = 0F38, 3 = 0F3A; XOP 8-10)
  uint8_t opcode = 0;   // the last opcode byte
  uint8_t opcode_offset = 0;

  // Legacy prefixes
  bool p66 = false;
  bool p67 = false;
  bool lock = false;
  uint8_t rep = 0;      // the last of F2 / F3, or 0
  uint8_t segment = 0;  // the segment override byte, or 0
  uint8_t rex = 0;      // 0x40-0x4F, or 0 when absent or cancelled by a later prefix

  // VEX / EVEX / XOP
  uint8_t vex_pp = 0;  // implied SIMD prefix: 0 none, 1 = 66, 2 = F3, 3 = F2
  bool vex_l = false;  // vector length above 128 bits
  bool vex_w = false;

  // ModRM / SIB / displacement
  bool has_modrm = false;
  uint8_t modrm = 0;
  uint8_t modrm_offset = 0;
  bool has_sib = false;
  uint8_t sib = 0;
  uint8_t disp_offset = 0;
  uint8_t disp_size = 0;
  bool rip_relative = false;

  // Immediates: all of them together (ENTER, EXTRQ and INSERTQ have two)
  uint8_t imm_offset = 0;
  uint8_t imm_size = 0;

  Flow flow = Flow::Next;
  int32_t branch_disp = 0;  // direct branches: target = address + length + branch_disp

  uint8_t mod() const { return static_cast<uint8_t>(modrm >> 6); }
  uint8_t reg() const { return static_cast<uint8_t>((modrm >> 3) & 7); }
  uint8_t rm() const { return static_cast<uint8_t>(modrm & 7); }
  bool rex_w() const { return (rex & 8) != 0; }
  bool memory_operand() const { return has_modrm && mod() != 3; }
  bool direct_branch() const { return flow == Flow::Jump || flow == Flow::CondJump || flow == Flow::Call; }
  // The SIMD prefix in VEX numbering, also for legacy encodings: 0 none, 1 = 66, 2 = F3, 3 = F2.
  uint8_t simd_prefix() const;
};

enum class DecodeStatus : uint8_t {
  Ok,
  Truncated,  // `code` ends inside the instruction
  Invalid,    // not a valid 64-bit-mode instruction, or longer than 15 bytes
};

DecodeStatus decode(std::span<const uint8_t> code, Instruction& out);

// What an instruction needs from the CPU beyond x86-64 with SSE2.
struct Requirement {
  // nullopt: the baseline, or SSE3 to SSE4.2 (which every CPU running Windows 11 24H2 has)
  std::optional<host::CpuFeature> feature;
  // An encoding the console's CPU does not have (AVX-512, XOP, 3DNow!, VAES, GFNI...). In console
  // code that means the bytes were data, not instructions.
  bool not_on_console = false;
};

Requirement requirement(const Instruction& insn);

}  // namespace am::x86
