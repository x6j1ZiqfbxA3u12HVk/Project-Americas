#pragma once

// HLSL into our DXIL: the subset the translator writes (its helpers, and programs in the goto
// form, where each console block is a label and its branches are gotos), compiled straight into a
// Module. One source of the translation's meaning (the HLSL the translator writes) for both ways
// to DXIL: DXC for programs whose branches nest, this for the ones HLSL cannot express (a console
// block reached from hundreds of places, nesting deeper than DXC's 256 levels), keeping the
// console's own control flow.
//
// What is taken: the preprocessor's #define (with arguments), #ifdef, #ifndef, #else, #endif;
// structs, cbuffers, bound and heap-indexed raw buffers, groupshared and static const arrays;
// bool, int, uint, 64-bit integers, half (32-bit, as without -enable-16bit-types), float,
// double, their vectors and arrays; functions (inlined at every call; in, out, inout and default
// arguments); if, for, while, do, switch, break, continue, return, labels and goto; HLSL 2021's
// conversion rules and its select(), and(), or(); the intrinsics and wave operations the
// translator uses. Anything else is refused with its line, never guessed.

#include <string>
#include <string_view>

#include "shader/dxil/dxil.h"

namespace am::shader::dxil {

// Compiles `source` into `m` with `entry` as the entry point; then m.finish() and write_bitcode().
// Float operations carry fast-math flags, as DXC's do at -O3.
// `profile` (cs_6_6, vs_6_6, ps_6_6, ms_6_6) gives the stage; empty: what the entry declares.
// `registers_in_memory`: the entry's register arrays (s, v) kept in memory (compute and mesh),
// for programs too large for the driver's compiler otherwise.
bool compile_hlsl(std::string_view source, std::string_view entry, Module& m, std::string& error, std::string_view profile = {},
                  bool registers_in_memory = false);

}  // namespace am::shader::dxil
