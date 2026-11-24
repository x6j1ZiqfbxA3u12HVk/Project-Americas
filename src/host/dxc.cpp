#include "host/dxc.h"

#include <windows.h>
// dxcapi.h needs the COM base types, which WIN32_LEAN_AND_MEAN leaves out of windows.h.
#include <unknwn.h>
#include <oleauto.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <array>
#include <cwchar>
#include <optional>
#include <system_error>

#include "core/file_view.h"
#include "core/text.h"

using Microsoft::WRL::ComPtr;

namespace am::host {
namespace {

std::filesystem::path windows_kits_root() {
  wchar_t buf[MAX_PATH] = {};
  DWORD size = sizeof buf;
  for (const DWORD view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}) {
    size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", L"KitsRoot10",
                     RRF_RT_REG_SZ | view, nullptr, buf, &size) == ERROR_SUCCESS) {
      return buf;
    }
  }
  return L"C:\\Program Files (x86)\\Windows Kits\\10\\";
}

// "10.0.26100.0" -> {10, 0, 26100, 0}; nullopt for other folder names.
std::optional<std::array<int, 4>> sdk_version(const std::wstring& name) {
  std::array<int, 4> v{};
  if (swscanf_s(name.c_str(), L"%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3]) != 4) return std::nullopt;
  return v;
}

}  // namespace

DxcInfo probe_dxc() {
  DxcInfo info;
  wchar_t env[1024];
  const DWORD n = GetEnvironmentVariableW(L"AMERICAS_DXC_DIR", env, static_cast<DWORD>(std::size(env)));
  if (n > 0 && n < std::size(env)) {
    info.dir = env;
    info.source = "AMERICAS_DXC_DIR";
  } else {
    const std::filesystem::path bin = windows_kits_root() / L"bin";
    std::optional<std::array<int, 4>> best;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(bin, ec)) {
      const auto v = sdk_version(entry.path().filename().wstring());
      if (!v || !std::filesystem::exists(entry.path() / L"x64" / L"dxcompiler.dll", ec)) continue;
      if (!best || *v > *best) {
        best = v;
        info.dir = entry.path() / L"x64";
        info.source = "Windows SDK " + narrow(entry.path().filename().wstring());
      }
    }
  }
  if (info.dir.empty()) {
    info.error = "no dxcompiler.dll found (install the Windows SDK or set AMERICAS_DXC_DIR)";
    return info;
  }
  std::error_code ec;
  info.separate_validator = std::filesystem::exists(info.dir / L"dxil.dll", ec);

  const std::filesystem::path dll = info.dir / L"dxcompiler.dll";
  HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) {
    info.error = "cannot load " + narrow(dll.wstring()) + ": " + win32_error_text(GetLastError());
    return info;
  }
  auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
  if (!create) {
    info.error = "dxcompiler.dll has no DxcCreateInstance";
    FreeLibrary(module);
    return info;
  }
  {
    ComPtr<IDxcVersionInfo> version;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&version)))) {
      info.error = "dxcompiler.dll refused to create a compiler";
    } else {
      info.found = true;
      ComPtr<IDxcVersionInfo3> version3;
      char* custom = nullptr;
      if (SUCCEEDED(version.As(&version3)) && SUCCEEDED(version3->GetCustomVersionString(&custom)) && custom) {
        info.version = custom;
        CoTaskMemFree(custom);
      } else {
        UINT32 major = 0, minor = 0;
        version->GetVersion(&major, &minor);
        info.version = std::to_string(major) + "." + std::to_string(minor);
      }
    }
  }
  FreeLibrary(module);
  return info;
}

}  // namespace am::host
