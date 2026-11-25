#pragma once

// The shader translator: console RDNA 2 programs to HLSL, compiled by DXC to DXIL (plan, 5.6).
//
// Version 1 emulates the wave faithfully, one HLSL thread per console lane, at the program's
// own wave size ([WaveSize]):
// * SGPRs, VCC, EXEC, M0 and SCC are variables that every lane holds alike; VGPRs are per lane;
// * EXEC is kept exactly: every vector write is predicated on the lane's EXEC bit, compares
//   build their lane masks with WaveActiveBallot;
// * branches on this GPU are always wave-uniform (scalar conditions), so control flow is a
//   dispatcher loop, a switch over block starts: correct for every program without
//   structuring. Structuring, EXEC removal and uniformity analysis come later, as speed;
// * memory goes through small functions of a binding model: `Test` maps GPU addresses onto one
//   flat buffer and dumps the registers at s_endpgm, for the GPU tests; the runtime's model
//   (the graphics layer's page table and descriptors) comes with M4.
//
// Every instruction a program reaches must be supported; the others are named, not guessed.

#include <cstdint>
#include <map>
#include <span>
#include <string>

#include "shader/console_program.h"

namespace am::shader {

// Bumped whenever the output for some input changes: part of the cache key.
constexpr uint32_t kTranslatorVersion = 2;

enum class Binding : uint8_t { Test, Runtime };

struct TranslateInput {
  std::span<const uint32_t> code;
  HwStage stage = HwStage::Cs;
  bool wave32 = false;
  uint32_t vgprs = 0;      // the record's count: at least the highest VGPR + 1
  uint32_t lds_bytes = 0;
  uint32_t cs_rsrc2 = 0;
  uint32_t cs_threads[3] = {1, 1, 1};
};

struct Translation {
  bool ok = false;
  std::string hlsl;
  std::string entry = "main";
  std::string profile = "cs_6_6";
  uint32_t wave_size = 64;
  uint32_t vgpr_count = 0;           // VGPRs the HLSL keeps per lane
  bool structured = false;           // nested ifs and loops (else one loop over a switch)
  std::string failure;               // the first reason it failed
  std::map<std::string, int> unsupported;  // instruction names (or reasons) and how often
};

Translation translate(const TranslateInput& in, Binding binding);

// For the tests' binding model: the dump's layout.
constexpr uint32_t kDumpSgprs = 0;     // 128 SGPRs, then SCC, written by thread 0 of group 0
constexpr uint32_t kDumpLanes = 1024;  // from here, each thread's VGPRs (vgpr_count dwords)

}  // namespace am::shader
