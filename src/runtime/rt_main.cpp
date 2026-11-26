// americas_rt.dll: parses the launcher's command line, loads the title and runs it.
//
//   americas [--title <exe>] [--force-relocation] [--unimplemented zero|trap]
//            [--allow-missing-cpu-features] [--console auto|series_s|series_x]
//            [--memory-limit <MiB>] [--timeout <seconds>] [--no-shader-service]
//            [-- <title arguments>]
//
// --memory-limit caps the process's committed memory (a job object, which counts private memory;
// the physical page pool holds itself to the same cap); --timeout ends the run after that many
// seconds, report written. Both are for unattended test runs. --console overrides americas.ini.
// --no-shader-service: do not start `am-shaders serve` (americas.ini shader_service = off).
//
// Without --title it runs the executable MicrosoftGame.Config names in game_dir (americas.ini).

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/config.h"
#include "core/text.h"
#include "host/system.h"
#include "runtime/console.h"
#include "runtime/files/console_paths.h"
#include "runtime/hle/registry.h"
#include "runtime/launcher_api.h"
#include "runtime/log.h"
#include "runtime/memory/title_memory.h"
#include "runtime/process.h"
#include "runtime/shader_service.h"
#include "runtime/title_config.h"

// The TLS carriers (runtime/tls.h): imported, so Windows loads them with their slots.
extern "C" __declspec(dllimport) void* am_tls_carrier_1();
extern "C" __declspec(dllimport) void* am_tls_carrier_2();
extern "C" __declspec(dllimport) void* am_tls_carrier_3();
extern "C" __declspec(dllimport) void* am_tls_carrier_4();

namespace {

using namespace am;

int usage() {
  std::fprintf(stderr,
               "usage: americas [--title <exe>] [--force-relocation] [--unimplemented zero|trap]\n"
               "                [--allow-missing-cpu-features] [--console auto|series_s|series_x]\n"
               "                [--memory-limit <MiB>] [--timeout <seconds>] [--no-shader-service]\n"
               "                [-- <title arguments>]\n");
  return 2;
}

void thread_hook(void* /*module*/, unsigned long reason) {
  if (rt::TitleProcess* p = rt::process()) p->on_thread(reason);
}

rt::TitleProcess* g_title = nullptr;

bool limit_memory(uint64_t mib) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  // Breakaway allowed: the shader service leaves the job (shader_service.h).
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
  limits.ProcessMemoryLimit = static_cast<SIZE_T>(mib) << 20;
  return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) &&
         AssignProcessToJobObject(job, GetCurrentProcess());
}

DWORD WINAPI watchdog(void* seconds) {
  Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(seconds)) * 1000);
  rt::logf("timeout: ending the run after %llu s", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(seconds)));
  if (g_title) {
    g_title->dump_threads();
    g_title->write_report();
  }
  TerminateProcess(GetCurrentProcess(), 3);
  return 0;
}

}  // namespace

extern "C" __declspec(dllexport) int am_launch(const rt::LauncherInfo* info) {
  if (!info || info->version != 2) return 2;
  volatile void* carriers[] = {am_tls_carrier_1(), am_tls_carrier_2(), am_tls_carrier_3(), am_tls_carrier_4()};
  (void)carriers;
  std::vector<std::string> warnings;
  const Config config = load_config(warnings);

  rt::RunOptions options;
  uint64_t memory_limit_mib = 0, timeout_s = 0;
  std::string console_setting = config.console;
  bool shader_service = config.shader_service;
  options.logs_dir = config.logs_dir();
  options.cache_dir = config.cache_dir();
  options.image_window = info->image_window;
  for (int i = 1; i < info->argc; ++i) {
    const std::wstring a = info->argv[i];
    if (a == L"--title" && i + 1 < info->argc) {
      options.title = info->argv[++i];
    } else if (a == L"--console" && i + 1 < info->argc) {
      console_setting = narrow(info->argv[++i]);
    } else if (a == L"--force-relocation") {
      options.force_relocation = true;
    } else if (a == L"--no-shader-service") {
      shader_service = false;
    } else if (a == L"--allow-missing-cpu-features") {
      options.allow_missing_cpu_features = true;
    } else if (a == L"--memory-limit" && i + 1 < info->argc) {
      memory_limit_mib = wcstoull(info->argv[++i], nullptr, 10);
      if (!memory_limit_mib) return usage();
    } else if (a == L"--timeout" && i + 1 < info->argc) {
      timeout_s = wcstoull(info->argv[++i], nullptr, 10);
      if (!timeout_s) return usage();
    } else if (a == L"--unimplemented" && i + 1 < info->argc) {
      const std::wstring mode = info->argv[++i];
      if (mode == L"zero") options.unimplemented = rt::UnimplementedMode::Zero;
      else if (mode == L"trap") options.unimplemented = rt::UnimplementedMode::Trap;
      else return usage();
    } else if (a == L"--") {
      for (++i; i < info->argc; ++i) options.args.push_back(info->argv[i]);
    } else {
      return usage();
    }
  }
  if (options.title.empty()) {
    if (config.game_dir.empty()) {
      std::fprintf(stderr, "no --title and no game_dir in %s\n", narrow(config.ini_path.wstring()).c_str());
      return usage();
    }
    options.title = rt::read_title_config(config.game_dir).executable;
    if (options.title.empty()) {
      std::fprintf(stderr, "MicrosoftGame.Config in %s names no executable\n", narrow(config.game_dir.wstring()).c_str());
      return 1;
    }
  }

  rt::log_open(options.logs_dir / L"run.log");
  for (const std::string& w : warnings) rt::logf("warning: %s", w.c_str());
  rt::logf("title %s", narrow(options.title.wstring()).c_str());
  rt::set_title_config(rt::read_title_config(options.title.parent_path()));
  rt::configure_shader_service(shader_service, config.shader_jobs, options.cache_dir, options.logs_dir);
  rt::files::configure({options.title.parent_path(), config.userdata_dir() / L"persistent", config.userdata_dir() / L"temp"});
  if (const rt::TitleConfig& tc = rt::title_config(); tc.title_id) {
    rt::logf("title id %08X, %s %s", tc.title_id, tc.identity_name.c_str(), tc.version.c_str());
  }
  if (memory_limit_mib) {
    if (!limit_memory(memory_limit_mib)) rt::logf("cannot limit memory: error %lu", GetLastError());
    else rt::logf("memory limited to %llu MiB", static_cast<unsigned long long>(memory_limit_mib));
  }
  if (!rt::mem::host_supports_placeholders()) {
    rt::logf("this Windows lacks placeholder memory (VirtualAlloc2, MapViewOfFile3): Windows 10 1803 or later is needed");
    return 1;
  }
  const host::MemoryInfo host_memory = host::memory_info();
  rt::Console console = rt::Console::SeriesS;
  if (!rt::choose_console(console_setting, host_memory.total_physical, &console)) return usage();
  rt::set_console(console);
  const rt::ConsoleProfile& profile = rt::current_console();
  rt::mem::MemoryOptions memory;
  memory.physical_bytes = profile.title_memory;
  memory.commit_cap = memory_limit_mib << 20;
  rt::mem::configure_title_memory(memory);
  rt::logf("console %s (setting %s): title memory %s; host RAM %s, commit limit %s", profile.name,
           console_setting.c_str(), format_bytes(profile.title_memory).c_str(),
           format_bytes(host_memory.total_physical).c_str(), format_bytes(host_memory.commit_limit).c_str());
  if (timeout_s) {
    CloseHandle(CreateThread(nullptr, 0, &watchdog, reinterpret_cast<void*>(static_cast<uintptr_t>(timeout_s)), 0, nullptr));
  }

  g_title = new rt::TitleProcess(options);  // lives until the process ends
  std::string error;
  if (!g_title->load(error)) {
    rt::logf("load failed: %s", error.c_str());
    g_title->write_report();
    return 1;
  }
  *info->thread_hook = &thread_hook;
  const int code = g_title->run();
  rt::logf("%s returned %d", g_title->executable()->name.c_str(), code);
  g_title->write_report();
  return code;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
  // The title usually ends the process itself (exit): keep its report.
  if (reason == DLL_PROCESS_DETACH && reserved && g_title) g_title->write_report();
  return TRUE;
}
