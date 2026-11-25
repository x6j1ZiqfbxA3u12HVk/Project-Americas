#pragma once

// A vertex or pixel shader's interface, for the translator (TranslateInput's inputs and
// outputs): from the container's signatures (ISG1, OSG1: semantic, index, register) and the
// console record's attribute maps, which start at record + 0x78:
//
//   VS  5-byte entries until a 0 type: type (6: 32-bit float), signature register, component
//       mask, the first VGPR the console's vertex fetch fills, components - 1.
//   PS  8-byte entries until a 0 type: type, signature register, component mask, attribute
//       number (the v_interp ATTR that reads it), four bytes not read yet.
//
// Outputs: a vertex shader's parameter N is its N-th output other than SV_Position, by
// register; a pixel shader's render target N is its SV_Target N.

#include <cstdint>
#include <span>
#include <string>

#include "shader/console_program.h"
#include "shader/translate.h"

namespace am::shader {

bool read_stage_io(std::span<const uint8_t> container, const NativeBlob& blob, const NativeProgram& program,
                   TranslateInput& in, std::string& error);

}  // namespace am::shader
