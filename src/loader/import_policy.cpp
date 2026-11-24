#include "loader/import_policy.h"

#include <algorithm>
#include <span>
#include <unordered_map>

#include "core/text.h"

namespace am::loader {
namespace {

constexpr std::string_view kConsoleOnly[] = {
    "acphal.dll",       // audio control processor
    "chad.dll",         // audio DSP
    "d3d12_x.dll",      // Direct3D 12.X (Xbox One generation)
    "d3d12_xs.dll",     // Direct3D 12.X (Series X|S)
    "gameinput.dll",    // console GameInput (ours, over the host's input)
    "kernelx.dll",      // console kernel
    "xapu.dll",         // audio processing unit
    "xcrdapi.dll",      // console DirectStorage
    "xg_x.dll",         // texture layout computer (Xbox One generation)
    "xg_xs.dll",        // texture layout computer (Series X|S)
    "xgameruntime.dll", // GDK services
    "xmem.dll",         // console memory manager
};

constexpr std::string_view kHostModules[] = {
    "advapi32.dll", "bcrypt.dll",     "combase.dll",  "concrt140.dll",   "crypt32.dll",  "dbghelp.dll",
    "dnsapi.dll",   "gdi32.dll",      "iphlpapi.dll", "kernel32.dll",    "kernelbase.dll", "mf.dll",
    "mfplat.dll",   "mfreadwrite.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "secur32.dll", "shell32.dll",
    "shlwapi.dll",  "ucrtbase.dll",   "user32.dll",   "vccorlib140.dll", "vcomp140.dll", "vcruntime140.dll",
    "vcruntime140_1.dll", "version.dll", "winhttp.dll", "ws2_32.dll", "xaudio2_9.dll",
};

// Host functions the title must not reach unchanged. Matched by name, whichever API set or DLL
// the title imports them from.
constexpr std::string_view kPathFunctions[] = {
    "CreateFileA", "CreateFileW", "CreateFile2", "CreateDirectoryA", "CreateDirectoryW", "CreateDirectoryExW",
    "DeleteFileA", "DeleteFileW", "RemoveDirectoryA", "RemoveDirectoryW", "FindFirstFileA", "FindFirstFileW",
    "FindFirstFileExA", "FindFirstFileExW", "GetFileAttributesA", "GetFileAttributesW", "GetFileAttributesExA",
    "GetFileAttributesExW", "SetFileAttributesA", "SetFileAttributesW", "MoveFileA", "MoveFileW", "MoveFileExA",
    "MoveFileExW", "CopyFileA", "CopyFileW", "CopyFileExW", "CopyFile2", "ReplaceFileW", "GetDiskFreeSpaceExA",
    "GetDiskFreeSpaceExW", "GetFullPathNameA", "GetFullPathNameW", "GetTempPathA", "GetTempPathW", "GetTempPath2W",
    "fopen", "fopen_s", "_wfopen", "_wfopen_s", "_fsopen", "_wfsopen", "freopen", "freopen_s", "_wfreopen",
    "_wfreopen_s", "_open", "_wopen", "_sopen_s", "_wsopen_s", "_stat64", "_wstat64", "_stat64i32", "_wstat64i32",
    "_access", "_waccess", "_access_s", "_waccess_s", "_mkdir", "_wmkdir", "_rmdir", "_wrmdir", "remove", "_wremove",
    "rename", "_wrename", "_unlink", "_wunlink", "_findfirst64", "_wfindfirst64", "_findfirst64i32",
    "_wfindfirst64i32", "_chdir", "_wchdir",
};
constexpr std::string_view kMemoryFunctions[] = {
    "VirtualAlloc", "VirtualAllocEx", "VirtualAlloc2", "VirtualAllocFromApp", "VirtualFree", "VirtualFreeEx",
    "VirtualQuery", "VirtualQueryEx", "VirtualProtect", "VirtualProtectEx",
};
constexpr std::string_view kModuleFunctions[] = {
    "LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW", "LoadPackagedLibrary", "GetModuleHandleA",
    "GetModuleHandleW", "GetModuleHandleExA", "GetModuleHandleExW", "GetProcAddress", "FreeLibrary",
    "FreeLibraryAndExitThread", "GetModuleFileNameA", "GetModuleFileNameW", "DisableThreadLibraryCalls",
};
constexpr std::string_view kThreadFunctions[] = {
    "CreateThread", "_beginthread", "_beginthreadex", "ExitThread", "_endthreadex", "SetThreadAffinityMask",
    "SetThreadGroupAffinity", "GetThreadGroupAffinity", "SetThreadIdealProcessor", "SetThreadIdealProcessorEx",
    "GetThreadIdealProcessorEx", "SetThreadSelectedCpuSets", "SetProcessAffinityMask", "GetProcessAffinityMask",
};
constexpr std::string_view kSystemFunctions[] = {
    "GlobalMemoryStatusEx", "GetLogicalProcessorInformation", "GetLogicalProcessorInformationEx", "GetSystemInfo",
    "GetNativeSystemInfo", "GetSystemCpuSetInformation", "GetActiveProcessorCount", "GetMaximumProcessorCount",
};
constexpr std::string_view kLifecycleFunctions[] = {
    "RegisterAppStateChangeNotification", "UnregisterAppStateChangeNotification",
    "RegisterAppConstrainedChangeNotification", "UnregisterAppConstrainedChangeNotification",
};
constexpr std::string_view kNetworkFunctions[] = {
    "connect", "WSAConnect", "WSAConnectByNameA", "WSAConnectByNameW", "sendto", "WSASendTo", "getaddrinfo",
    "GetAddrInfoW", "GetAddrInfoExW", "gethostbyname", "WinHttpOpen", "WinHttpConnect", "WinHttpSendRequest",
};
constexpr std::string_view kCrashFunctions[] = {
    "SetUnhandledExceptionFilter", "UnhandledExceptionFilter", "WerRegisterFile",
};
constexpr std::string_view kCommandLineFunctions[] = {
    "GetCommandLineA", "GetCommandLineW", "__p___argc", "__p___argv", "__p___wargv",
    "_get_narrow_winmain_command_line", "_get_wide_winmain_command_line", "_configure_narrow_argv",
    "_configure_wide_argv", "__getmainargs", "__wgetmainargs",
};

struct Wrap {
  std::string_view reason;
  std::span<const std::string_view> functions;
};

const Wrap kWraps[] = {
    {"console paths", kPathFunctions},
    {"title memory", kMemoryFunctions},
    {"module identity", kModuleFunctions},
    {"threads and processors", kThreadFunctions},
    {"system information", kSystemFunctions},
    {"app lifecycle", kLifecycleFunctions},
    {"network policy", kNetworkFunctions},
    {"crash handling", kCrashFunctions},
    {"command line", kCommandLineFunctions},
};

const std::unordered_map<std::string_view, std::string_view>& wrap_table() {
  static const auto table = [] {
    std::unordered_map<std::string_view, std::string_view> t;
    for (const Wrap& w : kWraps) {
      for (std::string_view f : w.functions) t.emplace(f, w.reason);
    }
    return t;
  }();
  return table;
}

bool in_list(std::string_view dll, std::span<const std::string_view> list) {
  return std::any_of(list.begin(), list.end(), [&](std::string_view m) { return iequals_ascii(m, dll); });
}

}  // namespace

const char* route_name(Route r) {
  switch (r) {
    case Route::Ours: return "ours";
    case Route::Wrap: return "wrap";
    case Route::Host: return "host";
    case Route::Title: return "title";
    case Route::Unresolved: return "unresolved";
  }
  return "?";
}

bool is_console_only_module(std::string_view dll) { return in_list(dll, kConsoleOnly); }

bool is_host_module(std::string_view dll) {
  return istarts_with_ascii(dll, "api-ms-win-") || istarts_with_ascii(dll, "ext-ms-win-") || in_list(dll, kHostModules);
}

std::string_view wrap_reason(std::string_view function) {
  const auto& table = wrap_table();
  const auto it = table.find(function);
  return it == table.end() ? std::string_view() : it->second;
}

ImportPolicy::ImportPolicy(std::vector<std::string> title_modules) {
  for (const std::string& m : title_modules) title_modules_.push_back(to_lower_ascii(m));
}

bool ImportPolicy::is_title_module(std::string_view dll) const {
  const std::string lower = to_lower_ascii(dll);
  return std::find(title_modules_.begin(), title_modules_.end(), lower) != title_modules_.end();
}

Route ImportPolicy::route(std::string_view dll, std::string_view function) const {
  if (is_console_only_module(dll)) return Route::Ours;
  // The title's own copy of a module (its C++ runtime, for one) wins over the host's.
  if (is_title_module(dll)) return Route::Title;
  if (is_host_module(dll)) {
    return (!function.empty() && !wrap_reason(function).empty()) ? Route::Wrap : Route::Host;
  }
  return Route::Unresolved;
}

}  // namespace am::loader
