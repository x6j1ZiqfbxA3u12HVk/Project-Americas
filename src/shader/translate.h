#pragma once

// The shader translator: console RDNA 2 programs to HLSL, compiled by DXC to DXIL (plan, 5.6).
//
// Version 1 emulates the wave faithfully, one HLSL thread per console lane (compute at the
// program's own wave size, [WaveSize]; vertex and pixel shaders at the host's):
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
#include <vector>

#include "shader/console_program.h"

namespace am::shader {

// Bumped whenever the output for some input changes: part of the cache key.
constexpr uint32_t kTranslatorVersion = 6;
// The shader-visible heap slot of a null raw UAV (reads give zero, writes are dropped): title
// memory a page without a GPU window maps to, so memory access needs no branch.
constexpr uint32_t kNullMemoryView = 65535;

enum class Binding : uint8_t { Test, Runtime };

// Graphics stages: the shader's interface, from the container's signatures and the console
// record (stage_io.h reads both).
struct StageInput {
  std::string semantic;
  uint32_t semantic_index = 0;
  uint32_t components = 4;
  uint32_t vgpr = 0;  // VS: the first VGPR the console's vertex fetch fills with it
};
struct StageOutput {
  std::string semantic;
  uint32_t semantic_index = 0;
};

struct TranslateInput {
  std::span<const uint32_t> code;
  HwStage stage = HwStage::Cs;
  bool wave32 = false;
  uint32_t vgprs = 0;      // the record's count: at least the highest VGPR + 1
  uint32_t lds_bytes = 0;
  uint32_t cs_rsrc2 = 0;
  uint32_t cs_threads[3] = {1, 1, 1};
  // VS: vertex attributes; PS: the attributes v_interp reads, attribute N = inputs[N].
  std::vector<StageInput> inputs;
  // VS: exp parameter N = outputs[N]; PS: exp mrt N = outputs[N] (an SV_Target).
  std::vector<StageOutput> outputs;
  uint32_t user_sgprs = 16;  // graphics: the user data SGPRs the binding fills (the runtime's: at most 20)
  // Mesh: the group's lanes, the most vertices and primitives (lane n exports vertex n and
  // primitive n; console_program.h), its user SGPRs (from s8: eight system SGPRs come first).
  // Its outputs (exp param N) are `outputs`, as a vertex shader's.
  uint32_t ms_threads = 0, ms_max_vertices = 0, ms_max_primitives = 0, ms_user_sgprs = 0;
};

struct Translation {
  bool ok = false;
  std::string hlsl;
  std::string entry = "main";
  std::string profile = "cs_6_6";
  uint32_t wave_size = 64;
  uint32_t vgpr_count = 0;           // VGPRs the HLSL keeps per lane
  bool structured = false;           // nested ifs and loops (else one loop over a switch)
  bool lanes = false;                // lanes as threads (else the wave's masks: see translate.cpp)
  int nested_depth = 0;              // the nested structured form's bracket depth (chains past 200)
  std::string failure;               // the first reason it failed
  std::map<std::string, int> unsupported;  // instruction names (or reasons) and how often
};

Translation translate(const TranslateInput& in, Binding binding);

// For the tests' binding model: the dump's layout.
constexpr uint32_t kDumpSgprs = 0;     // 128 SGPRs, then SCC, written by thread 0 of group 0
constexpr uint32_t kDumpLanes = 1024;  // from here, each thread's VGPRs (vgpr_count dwords)

// The HLSL a translated program with the runtime binding starts with (helpers, the binding:
// am_load, am_store over title memory, am_user root constants): for the runtime's own helper
// shaders, which use the same root signature.
std::string runtime_prelude();

}  // namespace am::shader
