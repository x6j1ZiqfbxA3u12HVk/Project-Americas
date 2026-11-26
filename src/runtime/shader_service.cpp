// Starting the shader service from a game run (see shader_service.h).

#include "runtime/shader_service.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/text.h"
#include "runtime/log.h"
#include "shader/service.h"

namespace am::rt {
namespace {

namespace fs = std::filesystem;

bool g_enabled = false;
unsigned g_jobs = 0;
fs::path g_cache, g_logs;
std::once_flag g_once;

fs::path own_dir() {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&own_dir), &m)) {
    return {};
  }
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(m, buf.data(), static_cast<DWORD>(buf.size()));
  if (!n || n >= buf.size()) return {};
  buf.resize(n);
  return fs::path(buf).parent_path();
}

// A build's identity: size and last write time (copies keep both).
std::string identity(const fs::path& f) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a)) return {};
  char out[48];
  std::snprintf(out, sizeof out, "%llx-%llx",
                (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
                (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime);
  return out;
}

bool running() {
  HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, shader::kServiceMutex);
  if (!h) return false;
  CloseHandle(h);
  return true;
}

// Asks the running service to stop: it finishes the compiles in flight, then ends, and the
// replacement started next waits for it (the event stays set until the replacement runs).
bool ask_to_stop() {
  HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, shader::kServiceStopEvent);
  if (!stop) return false;  // a service from before the stop event existed
  SetEvent(stop);
  CloseHandle(stop);
  return true;
}

bool launch(const fs::path& exe, const fs::path& work_dir, unsigned jobs, std::string& how) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  std::error_code ec;
  fs::create_directories(g_logs, ec);
  HANDLE log = CreateFileW((g_logs / L"serve.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) {
    how = "cannot open serve.txt";
    return false;
  }
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> attr_buf(size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof log, nullptr, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = log;
  si.StartupInfo.hStdError = log;
  si.lpAttributeList = attrs;
  std::wstring command = L"\"" + exe.wstring() + L"\" serve " + std::to_wstring(jobs);
  PROCESS_INFORMATION pi{};
  const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS;
  // Outside the run's job first, so the service outlives whatever started the game.
  how = "outside the run's job";
  if (ok) {
    ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                        work_dir.c_str(), &si.StartupInfo, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
      how = "inside the run's job (the job refuses breakaway: it ends with it)";
      ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, flags, nullptr, work_dir.c_str(),
                          &si.StartupInfo, &pi);
    }
    if (!ok) how = "CreateProcess failed, error " + std::to_string(GetLastError());
  } else {
    how = "cannot set up the handle list";
  }
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(log);
  if (ok) {
    how += ", process " + std::to_string(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  return ok;
}

void start_now() {
  std::error_code ec;
  if (!fs::exists(g_cache / L"shaders" / L"pack" / L"index.json", ec)) {
    logf("shader service: no shader corpus (am-shaders index), not started");
    return;
  }
  const fs::path built = own_dir() / L"am-shaders.exe";
  const std::string id = identity(built);
  if (id.empty()) {
    logf("shader service: no %s, not started", narrow(built.wstring()).c_str());
    return;
  }
  const fs::path dir = g_cache / L"shaders" / shader::kServiceFolder;
  if (running()) {
    std::string path;  // UTF-8
    std::ifstream in(dir / shader::kServiceRunningFile);
    std::getline(in, path);
    if (!path.empty() && identity(fs::path(widen(path))) == id) {
      logf("shader service: running already (%s)", path.c_str());
      return;
    }
    logf("shader service: the running one (%s) is another build: it finishes its compiles, then the new one takes over",
         path.empty() ? "unknown" : path.c_str());
    if (!ask_to_stop()) {
      logf("shader service: it cannot be asked to stop; left running");
      return;
    }
  }
  fs::create_directories(dir, ec);
  const fs::path copy = dir / widen("am-shaders-" + id + ".exe");
  if (!fs::exists(copy, ec)) {
    const fs::path tmp = dir / widen("am-shaders-" + id + ".tmp");
    fs::copy_file(built, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(tmp, copy, ec);
    if (ec) {
      logf("shader service: cannot copy am-shaders.exe to %s: %s", narrow(copy.wstring()).c_str(), ec.message().c_str());
      return;
    }
  }
  // Earlier builds' copies (the one in use, if any, refuses).
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::wstring name = e.path().filename().wstring();
    if (name.starts_with(L"am-shaders-") && e.path() != copy) {
      std::error_code rm;
      fs::remove(e.path(), rm);
    }
  }
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  // One compile per thread but one (DXC uses one thread a compile; the service runs below
  // normal priority, so the game's threads come first). Large compiles are limited by memory
  // in the service itself.
  const unsigned jobs = g_jobs ? g_jobs : std::max(1u, threads - 1);
  std::string how;
  if (launch(copy, dir, jobs, how)) {
    logf("shader service: started, %u jobs, %s; its log is %s", jobs, how.c_str(), narrow((g_logs / L"serve.txt").wstring()).c_str());
  } else {
    logf("shader service: not started: %s", how.c_str());
  }
}

}  // namespace

void configure_shader_service(bool enabled, unsigned jobs, const fs::path& cache_dir, const fs::path& logs_dir) {
  g_enabled = enabled;
  g_jobs = jobs;
  g_cache = cache_dir;
  g_logs = logs_dir;
}

void start_shader_service() {
  if (!g_enabled) return;
  std::call_once(g_once, [] { std::thread(start_now).detach(); });
}

}  // namespace am::rt
