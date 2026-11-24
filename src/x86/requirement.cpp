// Which CPU feature an instruction needs. Exact for the features a PC may lack and the loader
// must patch (SSE4a, SHA, CLWB, CLZERO, RDPID, RDPRU, MONITORX) and for the BMI, FMA, F16C,
// ADX, MOVBE, AES, PCLMULQDQ, RDRAND / RDSEED and XSAVE families. Within VEX encodings the split
// between AVX and AVX2 follows the operation and vector length; anything not listed as AVX2,
// FMA, F16C or BMI counts as AVX.

#include "x86/decoder.h"

namespace am::x86 {
namespace {

using F = host::CpuFeature;

bool in(uint8_t v, uint8_t lo, uint8_t hi) { return v >= lo && v <= hi; }

Requirement vex_requirement(const Instruction& d) {
  Requirement r;
  r.feature = F::AVX;
  const uint8_t op = d.opcode;
  const uint8_t pp = d.vex_pp;
  const bool wide = d.vex_l;
  switch (d.vex_map) {
    case 1: {
      // Integer operations on 256-bit registers are AVX2; the moves (6E/6F/7E/7F/D6/E7) stay AVX.
      const bool integer_op = pp == 1 && (in(op, 0x60, 0x6D) || in(op, 0x70, 0x76) || in(op, 0xD1, 0xD5) ||
                                          op == 0xD7 || in(op, 0xD8, 0xDF) || in(op, 0xE0, 0xE5) ||
                                          in(op, 0xE8, 0xEF) || in(op, 0xF1, 0xF6) || in(op, 0xF8, 0xFE));
      if (integer_op && wide) r.feature = F::AVX2;
      break;
    }
    case 2:
      if (op == 0xF2 && pp == 0) r.feature = F::BMI1;                     // ANDN
      else if (op == 0xF3 && pp == 0) r.feature = F::BMI1;                // BLSR / BLSMSK / BLSI
      else if (op == 0xF7 && pp == 0) r.feature = F::BMI1;                // BEXTR
      else if (op == 0xF5 && (pp == 0 || pp == 2 || pp == 3)) r.feature = F::BMI2;  // BZHI / PEXT / PDEP
      else if (op == 0xF6 && pp == 3) r.feature = F::BMI2;                // MULX
      else if (op == 0xF7 && pp != 0) r.feature = F::BMI2;                // SHLX / SARX / SHRX
      else if (op == 0x13 && pp == 1) r.feature = F::F16C;                // VCVTPH2PS
      else if (pp == 1 && (in(op, 0x96, 0x9F) || in(op, 0xA6, 0xAF) || in(op, 0xB6, 0xBF))) r.feature = F::FMA3;
      else if (pp == 1 && (op == 0x16 || op == 0x36 || in(op, 0x45, 0x47) || in(op, 0x58, 0x5A) || op == 0x78 ||
                           op == 0x79 || op == 0x8C || op == 0x8E || in(op, 0x90, 0x93))) {
        r.feature = F::AVX2;  // VPERMPS/VPERMD, variable shifts, integer broadcasts, masked moves, gathers
      } else if (pp == 1 && (op == 0x18 || op == 0x19) && !d.memory_operand()) {
        r.feature = F::AVX2;  // VBROADCASTSS / SD from a register
      } else if (pp == 1 && wide && (op <= 0x0B || in(op, 0x1C, 0x1E) || in(op, 0x20, 0x25) || in(op, 0x28, 0x2B) ||
                                     in(op, 0x30, 0x35) || in(op, 0x37, 0x40))) {
        r.feature = F::AVX2;  // 256-bit integer shuffles, packs, extensions, compares, multiplies
      } else if (pp == 1 && in(op, 0xDB, 0xDF)) {
        if (wide) r.not_on_console = true;  // VAES (Zen 3)
        else r.feature = F::AES;
      } else if (pp == 1 && op == 0xCF) {
        r.not_on_console = true;  // GFNI
      }
      break;
    case 3:
      if (op == 0xF0 && pp == 3) r.feature = F::BMI2;  // RORX
      else if (op == 0x1D && pp == 1) r.feature = F::F16C;  // VCVTPS2PH
      else if (pp == 1 && (op <= 0x02 || op == 0x38 || op == 0x39 || op == 0x46)) r.feature = F::AVX2;
      else if (pp == 1 && wide && (op == 0x0E || op == 0x0F || op == 0x42 || op == 0x4C)) r.feature = F::AVX2;
      else if (pp == 1 && op == 0x44) {
        if (wide) r.not_on_console = true;  // VPCLMULQDQ (Zen 3)
        else r.feature = F::PCLMULQDQ;
      } else if (pp == 1 && op == 0xDF) {
        r.feature = F::AES;
      } else if (pp == 1 && (op == 0xCE || op == 0xCF)) {
        r.not_on_console = true;  // GFNI
      }
      break;
    default:
      r.feature.reset();
      r.not_on_console = true;
      break;
  }
  return r;
}

}  // namespace

Requirement requirement(const Instruction& d) {
  Requirement r;
  const uint8_t op = d.opcode;
  const uint8_t pp = d.simd_prefix();  // 0 none, 1 = 66, 2 = F3, 3 = F2
  const bool mem = d.memory_operand();
  const uint8_t reg = d.reg();
  switch (d.map) {
    case Map::Primary:
      break;
    case Map::Map0F:
      switch (op) {
        case 0x01:
          if (d.modrm == 0xF9) r.feature = F::RDTSCP;
          else if (d.modrm == 0xFA || d.modrm == 0xFB) r.feature = F::MONITORX;  // MONITORX / MWAITX
          else if (d.modrm == 0xFC) r.feature = F::CLZERO;
          else if (d.modrm == 0xFD) r.feature = F::RDPRU;
          else if (d.modrm == 0xD0) r.feature = F::XSAVE;  // XGETBV
          break;
        case 0x0D:
          r.feature = F::PREFETCHW;  // PREFETCH / PREFETCHW
          break;
        case 0x0E:
          r.not_on_console = true;  // FEMMS
          break;
        case 0x2B:
          if (pp == 2 || pp == 3) r.feature = F::SSE4A;  // MOVNTSS / MOVNTSD
          break;
        case 0x78:
        case 0x79:
          if (pp == 1 || pp == 3) r.feature = F::SSE4A;  // EXTRQ / INSERTQ (no prefix: VMREAD / VMWRITE)
          break;
        case 0xAE:
          if (mem) {
            if (pp == 1 && reg == 6) r.feature = F::CLWB;
            else if (pp == 1 && reg == 7) r.feature = F::CLFLUSHOPT;
            else if (pp == 0 && (reg == 4 || reg == 5)) r.feature = F::XSAVE;  // XSAVE / XRSTOR
            else if (pp == 0 && reg == 6) r.feature = F::XSAVEOPT;
          }
          break;
        case 0xB8:
          if (pp == 2) r.feature = F::POPCNT;
          break;
        case 0xBC:
          if (pp == 2) r.feature = F::BMI1;  // TZCNT: silently BSF on CPUs without BMI1
          break;
        case 0xBD:
          if (pp == 2) r.feature = F::LZCNT;  // LZCNT: silently BSR on CPUs without it
          break;
        case 0xC7:
          if (!mem && reg == 6 && pp != 2) r.feature = F::RDRAND;
          else if (!mem && reg == 7 && pp == 2) r.feature = F::RDPID;
          else if (!mem && reg == 7) r.feature = F::RDSEED;
          else if (mem && reg == 1 && d.rex_w()) r.feature = F::CX16;
          else if (mem && reg == 4) r.feature = F::XSAVEC;
          else if (mem && (reg == 3 || reg == 5)) r.feature = F::XSAVES;  // XRSTORS / XSAVES
          break;
        default:
          break;
      }
      break;
    case Map::Map0F38:
      if (in(op, 0xC8, 0xCD) && pp == 0) r.feature = F::SHA;
      else if ((op == 0xF0 || op == 0xF1) && pp != 3) r.feature = F::MOVBE;  // with F2: CRC32 (SSE4.2)
      else if (op == 0xF6 && (pp == 1 || pp == 2)) r.feature = F::ADX;       // ADCX / ADOX
      else if (in(op, 0xDB, 0xDF) && pp == 1) r.feature = F::AES;
      break;
    case Map::Map0F3A:
      if (op == 0xCC && pp == 0) r.feature = F::SHA;  // SHA1RNDS4
      else if (op == 0xDF && pp == 1) r.feature = F::AES;
      else if (op == 0x44 && pp == 1) r.feature = F::PCLMULQDQ;
      else if (op == 0xCE || op == 0xCF) r.not_on_console = true;  // GF2P8AFFINE
      break;
    case Map::Vex:
      return vex_requirement(d);
    case Map::Evex:
    case Map::Xop:
    case Map::Amd3DNow:
      r.not_on_console = true;
      break;
  }
  return r;
}

}  // namespace am::x86
