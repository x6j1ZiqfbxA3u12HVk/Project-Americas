#pragma once

// DXGI formats: how many bytes a texel (or a compressed block) takes.

#include <cstdint>

namespace am::rt::d3d12x {

struct FormatSize {
  uint32_t bytes = 0;  // per texel, or per block for block-compressed formats; 0: unknown
  uint32_t block = 1;  // block width and height in texels (4 for BC formats)
};

FormatSize format_size(uint32_t dxgi_format);

}  // namespace am::rt::d3d12x
