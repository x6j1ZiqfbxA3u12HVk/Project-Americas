// What the title asks about itself: its module handles and file names, the modules it loads by
// name, its command line, its crash filter and its app lifecycle.

#include <windows.h>

#include <cstring>
#include <string>

#include "core/text.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"
#include "runtime/process.h"

namespace am::rt::hle {
namespace {

// ---- module identity ----

HMODULE handle_of(const MappedImage* m) { return reinterpret_cast<HMODULE>(m->base); }

HMODULE WINAPI my_GetModuleHandleW(LPCWSTR name) {
  TitleProcess* p = process();
  if (!name) return handle_of(p->executable());
  if (const MappedImage* m = p->find_module(narrow(name))) return handle_of(m);
  if (HMODULE pseudo = p->pseudo_module(narrow(name))) return pseudo;
  return GetModuleHandleW(name);
}

HMODULE WINAPI my_GetModuleHandleA(LPCSTR name) {
  if (!name) return handle_of(process()->executable());
  return my_GetModuleHandleW(widen(name).c_str());
}

BOOL WINAPI my_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* out) {
  TitleProcess* p = process();
  if (!out) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
    if (const MappedImage* m = p->module_containing(name)) {
      *out = handle_of(m);
      return TRUE;
    }
    return GetModuleHandleExW(flags, name, out);
  }
  if (!name) {
    *out = handle_of(p->executable());
    return TRUE;
  }
  if (const MappedImage* m = p->find_module(narrow(name))) {
    *out = handle_of(m);
    return TRUE;
  }
  if (HMODULE pseudo = p->pseudo_module(narrow(name))) {
    *out = pseudo;
    return TRUE;
  }
  return GetModuleHandleExW(flags, name, out);
}

DWORD copy_out(const std::wstring& s, LPWSTR buf, DWORD size) {
  if (!buf || size == 0) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  const DWORD n = static_cast<DWORD>(s.size());
  if (n >= size) {
    std::memcpy(buf, s.data(), (size - 1) * sizeof(wchar_t));
    buf[size - 1] = 0;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return size;
  }
  std::memcpy(buf, s.c_str(), (n + 1) * sizeof(wchar_t));
  SetLastError(ERROR_SUCCESS);
  return n;
}

DWORD WINAPI my_GetModuleFileNameW(HMODULE module, LPWSTR buf, DWORD size) {
  TitleProcess* p = process();
  const MappedImage* m = module ? p->module_containing(module) : p->executable();
  if (m) return copy_out(m->path.wstring(), buf, size);
  return GetModuleFileNameW(module, buf, size);
}

DWORD WINAPI my_GetModuleFileNameA(HMODULE module, LPSTR buf, DWORD size) {
  TitleProcess* p = process();
  const MappedImage* m = module ? p->module_containing(module) : p->executable();
  if (!m) return GetModuleFileNameA(module, buf, size);
  const std::string s = narrow(m->path.wstring());
  if (!buf || size == 0) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  const DWORD n = static_cast<DWORD>(s.size());
  if (n >= size) {
    std::memcpy(buf, s.data(), size - 1);
    buf[size - 1] = 0;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return size;
  }
  std::memcpy(buf, s.c_str(), n + 1);
  SetLastError(ERROR_SUCCESS);
  return n;
}

HMODULE load_by_name(const std::string& name, const wchar_t* wide_name, DWORD flags) {
  TitleProcess* p = process();
  std::string error;
  if (MappedImage* m = p->load_module(name, error)) return handle_of(m);
  if (!error.empty()) {
    logf("LoadLibrary(%s): %s", name.c_str(), error.c_str());
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
  }
  if (HMODULE pseudo = p->pseudo_module(name)) return pseudo;
  return LoadLibraryExW(wide_name, nullptr, flags);
}

HMODULE WINAPI my_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
  if (!name || file) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  return load_by_name(narrow(name), name, flags);
}
HMODULE WINAPI my_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
  if (!name || file) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  return load_by_name(name, widen(name).c_str(), flags);
}
HMODULE WINAPI my_LoadLibraryW(LPCWSTR name) { return my_LoadLibraryExW(name, nullptr, 0); }
HMODULE WINAPI my_LoadLibraryA(LPCSTR name) { return my_LoadLibraryExA(name, nullptr, 0); }

FARPROC WINAPI my_GetProcAddress(HMODULE module, LPCSTR name) {
  TitleProcess* p = process();
  const bool by_ordinal = IS_INTRESOURCE(name);
  const auto ordinal = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(name));
  const std::string fn = by_ordinal ? std::string() : std::string(name);
  if (const MappedImage* m = p->module_containing(module)) {
    if (reinterpret_cast<const uint8_t*>(module) == m->base) {
      return reinterpret_cast<FARPROC>(p->resolve_import(m->name, fn, ordinal, by_ordinal));
    }
  }
  if (const std::string* pseudo = p->pseudo_module_name(module)) {
    return reinterpret_cast<FARPROC>(p->resolve_import(*pseudo, fn, ordinal, by_ordinal));
  }
  // A host module: the title still gets our wrapper where we have one.
  wchar_t path[MAX_PATH];
  if (GetModuleFileNameW(module, path, MAX_PATH)) {
    const std::string dll = narrow(std::filesystem::path(path).filename().wstring());
    return reinterpret_cast<FARPROC>(p->resolve_import(dll, fn, ordinal, by_ordinal));
  }
  return GetProcAddress(module, name);
}

BOOL WINAPI my_FreeLibrary(HMODULE module) {
  TitleProcess* p = process();
  if (p->module_containing(module) || p->pseudo_module_name(module)) return TRUE;  // title modules stay mapped
  return FreeLibrary(module);
}

BOOL WINAPI my_DisableThreadLibraryCalls(HMODULE module) {
  if (MappedImage* m = process()->module_containing(module)) {
    m->thread_calls = false;
    return TRUE;
  }
  return DisableThreadLibraryCalls(module);
}

// Which image an address belongs to: the C++ runtime asks this to decode a thrown object's type.
PVOID NTAPI my_RtlPcToFileHeader(PVOID pc, PVOID* base) {
  if (const MappedImage* m = process()->module_containing(pc)) {
    *base = m->base;
    return m->base;
  }
  using Fn = PVOID(NTAPI*)(PVOID, PVOID*);
  static const auto real = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlPcToFileHeader"));
  return real(pc, base);
}

// ---- command line ----

LPSTR WINAPI my_GetCommandLineA() { return const_cast<LPSTR>(process()->command_line_a().c_str()); }
LPWSTR WINAPI my_GetCommandLineW() { return const_cast<LPWSTR>(process()->command_line_w().c_str()); }
int* __cdecl my___p___argc() { return process()->argc_ptr(); }
char*** __cdecl my___p___argv() { return process()->argv_ptr(); }
wchar_t*** __cdecl my___p___wargv() { return process()->wargv_ptr(); }
int __cdecl my__configure_argv(int /*mode*/) { return 0; }  // our argv is ready already
char* __cdecl my__get_narrow_winmain_command_line() { return process()->winmain_args_a(); }
wchar_t* __cdecl my__get_wide_winmain_command_line() { return process()->winmain_args_w(); }

// ---- crash handling ----

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI my_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  return process()->set_title_crash_filter(filter);
}
LONG WINAPI my_UnhandledExceptionFilter(EXCEPTION_POINTERS* info) { return process()->crash_filter(info); }
HRESULT WINAPI my_WerRegisterFile(PCWSTR file, int /*type*/, DWORD /*flags*/) {
  logf("WerRegisterFile(%s) ignored: crash reports stay local", file ? narrow(file).c_str() : "");
  return S_OK;
}

// ---- app lifecycle: the title is always running and never constrained ----

ULONG WINAPI my_RegisterAppStateChangeNotification(void* /*routine*/, void* /*context*/, void** registration) {
  static char token;
  if (registration) *registration = &token;
  return ERROR_SUCCESS;
}
void WINAPI my_UnregisterAppStateChangeNotification(void* /*registration*/) {}
ULONG WINAPI my_RegisterAppConstrainedChangeNotification(void* routine, void* context, void** registration) {
  return my_RegisterAppStateChangeNotification(routine, context, registration);
}
void WINAPI my_UnregisterAppConstrainedChangeNotification(void* /*registration*/) {}

#define AM_WRAP(name) Function{nullptr, #name, reinterpret_cast<void*>(&my_##name)}

}  // namespace

void add_identity(std::vector<Function>& out) {
  out.insert(out.end(), {
      AM_WRAP(GetModuleHandleW), AM_WRAP(GetModuleHandleA), AM_WRAP(GetModuleHandleExW), AM_WRAP(GetModuleFileNameW),
      AM_WRAP(GetModuleFileNameA), AM_WRAP(LoadLibraryExW), AM_WRAP(LoadLibraryExA), AM_WRAP(LoadLibraryW),
      AM_WRAP(LoadLibraryA), AM_WRAP(GetProcAddress), AM_WRAP(FreeLibrary), AM_WRAP(DisableThreadLibraryCalls),
      AM_WRAP(RtlPcToFileHeader),
      AM_WRAP(GetCommandLineA), AM_WRAP(GetCommandLineW), AM_WRAP(__p___argc), AM_WRAP(__p___argv),
      AM_WRAP(__p___wargv), Function{nullptr, "_configure_narrow_argv", reinterpret_cast<void*>(&my__configure_argv)},
      Function{nullptr, "_configure_wide_argv", reinterpret_cast<void*>(&my__configure_argv)},
      AM_WRAP(_get_narrow_winmain_command_line), AM_WRAP(_get_wide_winmain_command_line),
      AM_WRAP(SetUnhandledExceptionFilter), AM_WRAP(UnhandledExceptionFilter), AM_WRAP(WerRegisterFile),
      AM_WRAP(RegisterAppStateChangeNotification), AM_WRAP(UnregisterAppStateChangeNotification),
      AM_WRAP(RegisterAppConstrainedChangeNotification), AM_WRAP(UnregisterAppConstrainedChangeNotification),
  });
}

}  // namespace am::rt::hle
