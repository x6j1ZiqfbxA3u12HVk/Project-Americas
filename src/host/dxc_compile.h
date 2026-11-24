#pragma once

// Compiling HLSL to DXIL with the DirectX shader compiler found by probe_dxc().

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace am::host {

struct CompileResult {
  bool ok = false;
  std::vector<uint8_t> dxil;  // the signed container, ready for D3D12
  std::string messages;       // errors and warnings
};

// `profile` like "cs_6_6". Thread-safe: each call makes its own compiler object.
CompileResult compile_hlsl(std::string_view source, std::string_view entry, std::string_view profile,
                           const std::vector<std::wstring>& extra_args = {});

// Whether a compiler was found (probe_dxc), and why not.
bool dxc_available(std::string* why = nullptr);

}  // namespace am::host
