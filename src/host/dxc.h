#pragma once

#include <filesystem>
#include <string>

namespace am::host {

// The DirectX shader compiler the shader recompiler hands its output to.
struct DxcInfo {
  bool found = false;
  std::filesystem::path dir;  // folder with dxcompiler.dll
  std::string source;         // "AMERICAS_DXC_DIR" or "Windows SDK <version>"
  std::string version;        // e.g. "1.9.2602.17 (21d28f727)"
  bool separate_validator = false;  // dxil.dll next to it
  std::string error;
};

// Looks in %AMERICAS_DXC_DIR%, then in the newest Windows SDK's bin\<version>\x64, loads the
// compiler and asks it for its version.
DxcInfo probe_dxc();

}  // namespace am::host
