#include "host/system.h"

#include <windows.h>
#include <psapi.h>

#include <bit>
#include <vector>

namespace am::host {
namespace {

bool token_has_privilege(const wchar_t* name) {
  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, name, &luid)) return false;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  bool found = false;
  DWORD len = 0;
  GetTokenInformation(token, TokenPrivileges, nullptr, 0, &len);
  if (len) {
    std::vector<uint8_t> buf(len);
    if (GetTokenInformation(token, TokenPrivileges, buf.data(), len, &len)) {
      const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
      for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        const LUID& l = privileges->Privileges[i].Luid;
        if (l.LowPart == luid.LowPart && l.HighPart == luid.HighPart) found = true;
      }
    }
  }
  CloseHandle(token);
  return found;
}

}  // namespace

OsVersion os_version() {
  OsVersion v;
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
      RTL_OSVERSIONINFOW info{};
      info.dwOSVersionInfoSize = sizeof info;
      if (fn(&info) == 0) {
        v.major = info.dwMajorVersion;
        v.minor = info.dwMinorVersion;
        v.build = info.dwBuildNumber;
      }
    }
  }
  DWORD ubr = 0;
  DWORD size = sizeof ubr;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                   nullptr, &ubr, &size) == ERROR_SUCCESS) {
    v.revision = ubr;
  }
  return v;
}

Topology processor_topology() {
  Topology t;
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &len);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return t;
  std::vector<uint8_t> buf(len);
  auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationAll, first, &len)) return t;
  for (DWORD off = 0; off < len;) {
    const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    if (info->Size == 0) break;
    switch (info->Relationship) {
      case RelationProcessorCore:
        ++t.cores;
        for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
          t.logical += static_cast<uint32_t>(std::popcount(static_cast<unsigned long long>(info->Processor.GroupMask[g].Mask)));
        }
        break;
      case RelationProcessorPackage:
        ++t.packages;
        break;
      case RelationCache:
        if (info->Cache.Level == 3) {
          ++t.l3_caches;
          t.l3_bytes += info->Cache.CacheSize;
        }
        break;
      default:
        break;
    }
    off += info->Size;
  }
  return t;
}

MemoryInfo memory_info() {
  MemoryInfo m;
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof status;
  if (GlobalMemoryStatusEx(&status)) {
    m.total_physical = status.ullTotalPhys;
    m.available_physical = status.ullAvailPhys;
  }
  PERFORMANCE_INFORMATION perf{};
  perf.cb = sizeof perf;
  if (GetPerformanceInfo(&perf, sizeof perf)) {
    m.page_size = perf.PageSize;
    m.commit_limit = uint64_t{perf.CommitLimit} * perf.PageSize;
    m.commit_total = uint64_t{perf.CommitTotal} * perf.PageSize;
  }
  m.large_page_minimum = GetLargePageMinimum();
  m.lock_pages_privilege = token_has_privilege(SE_LOCK_MEMORY_NAME);
  return m;
}

}  // namespace am::host
