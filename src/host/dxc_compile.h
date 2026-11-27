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
  // The compiler could not run the compile at all (its Compile call failed: out of memory, for
  // one), as opposed to refusing the source: worth trying again later.
  bool transient = false;
};

// `profile` like "cs_6_6". Thread-safe: each call makes its own compiler object.
CompileResult compile_hlsl(std::string_view source, std::string_view entry, std::string_view profile,
                           const std::vector<std::wstring>& extra_args = {});

// A DXIL module (LLVM 3.7 bitcode) made into a validated, signed container.
CompileResult assemble_dxil(const std::vector<uint8_t>& module);

// DXC's LLVM optimizer over a module's bitcode (LLVM pass options such as "-instcombine"): the
// optimized bitcode in `dxil`, for assemble_dxil.
CompileResult optimize_dxil(const std::vector<uint8_t>& module, const std::vector<std::wstring>& passes);

// Whether a compiler was found (probe_dxc), and why not.
bool dxc_available(std::string* why = nullptr);

}  // namespace am::host
