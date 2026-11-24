#include "host/dxc_compile.h"

#include <windows.h>
// dxcapi.h needs the COM base types, which WIN32_LEAN_AND_MEAN leaves out of windows.h.
#include <unknwn.h>
#include <oleauto.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <mutex>

#include "core/text.h"
#include "host/dxc.h"

using Microsoft::WRL::ComPtr;

namespace am::host {
namespace {

struct Loaded {
  DxcCreateInstanceProc create = nullptr;
  std::string why;
};

// The compiler stays loaded for the process's life.
const Loaded& loaded() {
  static const Loaded l = [] {
    Loaded x;
    const DxcInfo info = probe_dxc();
    if (!info.found) {
      x.why = info.error.empty() ? "no shader compiler" : info.error;
      return x;
    }
    // dxil.dll, the validator and signer, must load from the same folder.
    SetDllDirectoryW(info.dir.c_str());
    HMODULE m = LoadLibraryExW((info.dir / L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (info.separate_validator) LoadLibraryExW((info.dir / L"dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    SetDllDirectoryW(nullptr);
    if (m) x.create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance"));
    if (!x.create) x.why = "cannot load dxcompiler.dll from " + narrow(info.dir.wstring());
    return x;
  }();
  return l;
}

}  // namespace

bool dxc_available(std::string* why) {
  const Loaded& l = loaded();
  if (why) *why = l.why;
  return l.create != nullptr;
}

CompileResult compile_hlsl(std::string_view source, std::string_view entry, std::string_view profile,
                           const std::vector<std::wstring>& extra_args) {
  CompileResult r;
  const Loaded& l = loaded();
  if (!l.create) {
    r.messages = l.why;
    return r;
  }
  ComPtr<IDxcCompiler3> compiler;
  ComPtr<IDxcUtils> utils;
  if (FAILED(l.create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || FAILED(l.create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
    r.messages = "cannot create the compiler";
    return r;
  }
  const std::wstring wentry = widen(entry), wprofile = widen(profile);
  std::vector<LPCWSTR> args = {L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-HV", L"2021", L"-O3"};
  for (const std::wstring& a : extra_args) args.push_back(a.c_str());
  DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
  ComPtr<IDxcResult> result;
  if (FAILED(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), nullptr, IID_PPV_ARGS(&result)))) {
    r.messages = "the compiler failed";
    return r;
  }
  ComPtr<IDxcBlobUtf8> errors;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors && errors->GetStringLength()) {
    r.messages.assign(errors->GetStringPointer(), errors->GetStringLength());
  }
  HRESULT status = E_FAIL;
  result->GetStatus(&status);
  ComPtr<IDxcBlob> object;
  if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object) {
    const auto* p = static_cast<const uint8_t*>(object->GetBufferPointer());
    r.dxil.assign(p, p + object->GetBufferSize());
    r.ok = true;
  }
  return r;
}

}  // namespace am::host
