// The title's file functions on console paths (runtime/files/console_paths.h): the title's
// folder read-only, S:\ and T:\ in userdata, nothing else. Opened handles are the host's own, so
// reads run at native speed; each one's final path is checked once at open.

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "core/text.h"
#include "runtime/files/console_paths.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::files {
namespace {

std::unique_ptr<PathMap> g_map;
std::mutex g_cwd_lock;  // _chdir changes the map's current directory

const PathMap& map() { return *g_map; }

std::atomic<int> g_logged{0};
void note(const char* what, std::wstring_view path, const Resolved& r, bool ok) {
  // Every refusal and the first opens are worth a line; the game opens thousands of files.
  if (ok && g_logged.load(std::memory_order_relaxed) > 300) return;
  if (g_logged.fetch_add(1, std::memory_order_relaxed) > 2000) return;
  logf("files: %s %s -> %s%s%s", what, narrow(path).c_str(), ok ? area_name(r.area) : "refused",
       ok ? "" : ": ", ok ? "" : r.why.c_str());
}

// Resolves for reading (`write` false) or for anything that changes the file system.
bool check(const char* what, std::wstring_view path, bool write, Resolved& r) {
  {
    std::lock_guard<std::mutex> lock(g_cwd_lock);
    r = map().resolve(path);
  }
  if (r.area != Area::Denied && write && !r.writable()) {
    r.area = Area::Denied;
    r.why = "the title's folder is read-only";
  }
  const bool ok = r.area != Area::Denied;
  note(what, path, r, ok);
  if (!ok) SetLastError(write ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND);
  return ok;
}
bool check(const char* what, const char* path, bool write, Resolved& r) {
  return check(what, path ? widen(path) : std::wstring(), write, r);
}

// A handle's final path must still be where its name pointed (links, junctions).
bool final_path_ok(HANDLE h, const Resolved& r) {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), FILE_NAME_NORMALIZED);
  if (!n || n >= std::size(buf)) return false;
  std::wstring_view p(buf, n);
  if (p.starts_with(L"\\\\?\\UNC\\")) return false;
  if (p.starts_with(L"\\\\?\\")) p.remove_prefix(4);
  return map().inside(r, std::filesystem::path(p));
}

constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | FILE_WRITE_DATA | FILE_APPEND_DATA |
                               FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;

bool wants_write(DWORD access, DWORD disposition, DWORD flags) {
  return (access & kWriteAccess) || disposition != OPEN_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE);
}

HANDLE opened(HANDLE h, const Resolved& r) {
  if (h == INVALID_HANDLE_VALUE) return h;
  if (!final_path_ok(h, r)) {
    CloseHandle(h);
    logf("files: %s resolves outside its area; refused", narrow(r.host.wstring()).c_str());
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }
  return h;
}

// ---- Win32 ----

HANDLE WINAPI my_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                             DWORD flags, HANDLE templ) {
  Resolved r;
  if (!check("open", name ? name : L"", wants_write(access, disposition, flags), r)) return INVALID_HANDLE_VALUE;
  return opened(CreateFileW(r.host.c_str(), access, share, sa, disposition, flags, templ), r);
}
HANDLE WINAPI my_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                             DWORD flags, HANDLE templ) {
  return my_CreateFileW(name ? widen(name).c_str() : nullptr, access, share, sa, disposition, flags, templ);
}
HANDLE WINAPI my_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                             LPCREATEFILE2_EXTENDED_PARAMETERS params) {
  Resolved r;
  const DWORD flags = params ? params->dwFileFlags : 0;
  if (!check("open", name ? name : L"", wants_write(access, disposition, flags), r)) return INVALID_HANDLE_VALUE;
  return opened(CreateFile2(r.host.c_str(), access, share, disposition, params), r);
}
DWORD WINAPI my_GetFileAttributesW(LPCWSTR name) {
  Resolved r;
  if (!check("attributes", name ? name : L"", false, r)) return INVALID_FILE_ATTRIBUTES;
  return GetFileAttributesW(r.host.c_str());
}
DWORD WINAPI my_GetFileAttributesA(LPCSTR name) {
  return my_GetFileAttributesW(name ? widen(name).c_str() : nullptr);
}
BOOL WINAPI my_GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
  Resolved r;
  if (!check("attributes", name ? name : L"", false, r)) return FALSE;
  return GetFileAttributesExW(r.host.c_str(), level, info);
}
BOOL WINAPI my_SetFileAttributesW(LPCWSTR name, DWORD attributes) {
  Resolved r;
  if (!check("set attributes", name ? name : L"", true, r)) return FALSE;
  return SetFileAttributesW(r.host.c_str(), attributes);
}
HANDLE WINAPI my_FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op,
                                  LPVOID filter, DWORD flags) {
  Resolved r;
  if (!check("find", pattern ? pattern : L"", false, r)) return INVALID_HANDLE_VALUE;
  return FindFirstFileExW(r.host.c_str(), level, data, op, filter, flags);
}
HANDLE WINAPI my_FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
  return my_FindFirstFileExW(pattern, FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0);
}
BOOL WINAPI my_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) {
  Resolved r;
  if (!check("create directory", name ? name : L"", true, r)) return FALSE;
  return CreateDirectoryW(r.host.c_str(), sa);
}
BOOL WINAPI my_RemoveDirectoryW(LPCWSTR name) {
  Resolved r;
  if (!check("remove directory", name ? name : L"", true, r)) return FALSE;
  return RemoveDirectoryW(r.host.c_str());
}
BOOL WINAPI my_DeleteFileW(LPCWSTR name) {
  Resolved r;
  if (!check("delete", name ? name : L"", true, r)) return FALSE;
  return DeleteFileW(r.host.c_str());
}
BOOL WINAPI my_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  Resolved a, b;
  if (!check("move", from ? from : L"", true, a)) return FALSE;
  if (to && !check("move to", to, true, b)) return FALSE;
  return MoveFileExW(a.host.c_str(), to ? b.host.c_str() : nullptr, flags);
}
HRESULT WINAPI my_CopyFile2(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* params) {
  Resolved a, b;
  if (!check("copy", from ? from : L"", false, a) || !check("copy to", to ? to : L"", true, b)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return CopyFile2(a.host.c_str(), b.host.c_str(), params);
}
BOOL WINAPI my_GetDiskFreeSpaceExW(LPCWSTR dir, PULARGE_INTEGER free_to_caller, PULARGE_INTEGER total,
                                   PULARGE_INTEGER total_free) {
  Resolved r;
  if (!check("free space", dir ? dir : L"", false, r)) return FALSE;
  return GetDiskFreeSpaceExW(r.host.c_str(), free_to_caller, total, total_free);
}
// The console's temp folder.
DWORD WINAPI my_GetTempPathW(DWORD size, LPWSTR buffer) {
  static const wchar_t kTemp[] = L"T:\\";
  const DWORD n = static_cast<DWORD>(std::size(kTemp) - 1);
  if (!buffer || size <= n) return n + 1;
  std::memcpy(buffer, kTemp, sizeof kTemp);
  return n;
}

// ---- C runtime ----

bool mode_writes(const char* mode) {
  for (const char* m = mode; m && *m; ++m) {
    if (*m == 'w' || *m == 'a' || *m == '+') return true;
  }
  return false;
}
bool mode_writes(const wchar_t* mode) {
  for (const wchar_t* m = mode; m && *m; ++m) {
    if (*m == L'w' || *m == L'a' || *m == L'+') return true;
  }
  return false;
}
FILE* stream_opened(FILE* f, const Resolved& r) {
  if (!f) return f;
  if (!final_path_ok(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))), r)) {
    std::fclose(f);
    errno = EACCES;
    return nullptr;
  }
  return f;
}
FILE* __cdecl my__wfsopen(const wchar_t* name, const wchar_t* mode, int share) {
  Resolved r;
  if (!check("fopen", name ? name : L"", mode_writes(mode), r)) {
    errno = ENOENT;
    return nullptr;
  }
  return stream_opened(_wfsopen(r.host.c_str(), mode, share), r);
}
FILE* __cdecl my__wfopen(const wchar_t* name, const wchar_t* mode) { return my__wfsopen(name, mode, _SH_DENYNO); }
FILE* __cdecl my__fsopen(const char* name, const char* mode, int share) {
  return my__wfsopen(name ? widen(name).c_str() : nullptr, mode ? widen(mode).c_str() : nullptr, share);
}
FILE* __cdecl my_fopen(const char* name, const char* mode) { return my__fsopen(name, mode, _SH_DENYNO); }

int __cdecl my__wopen(const wchar_t* name, int oflag, int pmode) {
  Resolved r;
  const bool write = oflag & (_O_WRONLY | _O_RDWR | _O_CREAT | _O_TRUNC | _O_APPEND);
  if (!check("open", name ? name : L"", write, r)) {
    errno = write ? EACCES : ENOENT;
    return -1;
  }
  int fd = -1;  // _wopen is _wsopen with no sharing restriction
  if (_wsopen_s(&fd, r.host.c_str(), oflag, _SH_DENYNO, pmode) != 0) return -1;
  if (fd >= 0 && !final_path_ok(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), r)) {
    _close(fd);
    errno = EACCES;
    return -1;
  }
  return fd;
}
int __cdecl my__open(const char* name, int oflag, int pmode) { return my__wopen(name ? widen(name).c_str() : nullptr, oflag, pmode); }

int __cdecl my__wstat64(const wchar_t* name, struct _stat64* out) {
  Resolved r;
  if (!check("stat", name ? name : L"", false, r)) {
    errno = ENOENT;
    return -1;
  }
  return _wstat64(r.host.c_str(), out);
}
int __cdecl my__wremove(const wchar_t* name) {
  Resolved r;
  if (!check("remove", name ? name : L"", true, r)) {
    errno = EACCES;
    return -1;
  }
  return _wremove(r.host.c_str());
}
int __cdecl my_remove(const char* name) { return my__wremove(name ? widen(name).c_str() : nullptr); }
int __cdecl my__wrename(const wchar_t* from, const wchar_t* to) {
  Resolved a, b;
  if (!check("rename", from ? from : L"", true, a) || !check("rename to", to ? to : L"", true, b)) {
    errno = EACCES;
    return -1;
  }
  return _wrename(a.host.c_str(), b.host.c_str());
}
int __cdecl my__wrmdir(const wchar_t* name) {
  Resolved r;
  if (!check("rmdir", name ? name : L"", true, r)) {
    errno = EACCES;
    return -1;
  }
  return _wrmdir(r.host.c_str());
}
// Relative paths resolve from the new directory; the process's own stays where it is.
int __cdecl my__wchdir(const wchar_t* name) {
  Resolved r;
  if (!check("chdir", name ? name : L"", false, r) || GetFileAttributesW(r.host.c_str()) == INVALID_FILE_ATTRIBUTES) {
    errno = ENOENT;
    return -1;
  }
  std::lock_guard<std::mutex> lock(g_cwd_lock);
  g_map->set_current(r);
  return 0;
}

#define AM_FILE(name) hle::Function{nullptr, #name, reinterpret_cast<void*>(&my_##name)}

}  // namespace

void* open_for_reading(const wchar_t* path) {
  return my_CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void configure(const Roots& roots) {
  std::error_code ec;
  std::filesystem::create_directories(roots.persistent, ec);
  std::filesystem::create_directories(roots.temp, ec);
  g_map = std::make_unique<PathMap>(roots);
  logf("files: title %s (read-only), S:\\ %s, T:\\ %s", narrow(g_map->roots().title.wstring()).c_str(),
       narrow(g_map->roots().persistent.wstring()).c_str(), narrow(g_map->roots().temp.wstring()).c_str());
}

void add_files(std::vector<hle::Function>& out) {
  out.insert(out.end(), {
      AM_FILE(CreateFileW), AM_FILE(CreateFileA), AM_FILE(CreateFile2), AM_FILE(GetFileAttributesW),
      AM_FILE(GetFileAttributesA), AM_FILE(GetFileAttributesExW), AM_FILE(SetFileAttributesW),
      AM_FILE(FindFirstFileExW), AM_FILE(FindFirstFileW), AM_FILE(CreateDirectoryW), AM_FILE(RemoveDirectoryW),
      AM_FILE(DeleteFileW), AM_FILE(MoveFileExW), AM_FILE(CopyFile2), AM_FILE(GetDiskFreeSpaceExW),
      AM_FILE(GetTempPathW), AM_FILE(_wfsopen), AM_FILE(_wfopen), AM_FILE(_fsopen), AM_FILE(fopen), AM_FILE(_wopen),
      AM_FILE(_open), AM_FILE(_wstat64), AM_FILE(_wremove), AM_FILE(remove), AM_FILE(_wrename), AM_FILE(_wrmdir),
      AM_FILE(_wchdir),
  });
}

}  // namespace am::rt::files
