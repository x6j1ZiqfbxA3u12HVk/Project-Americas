#pragma once

// The title running in our process: its modules, how their imports are bound, and the state the
// wrappers answer from (module identity, command line, crash filter).

#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "loader/import_policy.h"
#include "runtime/cpu_compat.h"
#include "runtime/hle/registry.h"
#include "runtime/image.h"
#include "runtime/stubs.h"
#include "runtime/tls.h"

namespace am::rt {

struct RunOptions {
  std::filesystem::path title;     // the title's executable
  std::vector<std::wstring> args;  // its arguments
  bool force_relocation = false;   // map modules away from their preferred bases (tests)
  UnimplementedMode unimplemented = UnimplementedMode::Zero;
  bool allow_missing_cpu_features = false;  // start even if the title needs instructions this CPU lacks
  std::filesystem::path logs_dir;
  std::filesystem::path cache_dir;
  void* image_window = nullptr;  // the launcher's hold on the executable's range, released to map it
};

class TitleProcess {
 public:
  explicit TitleProcess(RunOptions options);
  TitleProcess(const TitleProcess&) = delete;
  TitleProcess& operator=(const TitleProcess&) = delete;

  // Maps and binds the executable and the title DLLs it needs, installs TLS, protects the images
  // and initializes the DLLs.
  bool load(std::string& error);
  // Runs the executable's TLS callbacks and entry point. Usually does not return: the title exits.
  int run();

  // ---- for the wrappers ----
  MappedImage* executable() { return exe_; }
  const std::filesystem::path& cache_dir() const { return options_.cache_dir; }
  MappedImage* find_module(std::string_view name);
  // Lock-free: also asked during exception dispatch and crash reports.
  MappedImage* module_containing(const void* address);
  // A title DLL by name: mapped, bound and initialized if it was not yet. nullptr with an empty
  // `error` when `name` is not a title module.
  MappedImage* load_module(std::string_view name, std::string& error);
  // Console modules we implement get stand-in handles for LoadLibrary / GetModuleHandle.
  HMODULE pseudo_module(std::string_view name);
  const std::string* pseudo_module_name(HMODULE handle);
  // The address the title gets for `dll!function`, by the import policy.
  void* resolve_import(std::string_view dll, std::string_view function, uint16_t ordinal, bool by_ordinal);
  // Thread start / exit, from the launcher's TLS callback.
  void on_thread(unsigned long reason);

  const std::string& command_line_a() const { return command_line_a_; }
  const std::wstring& command_line_w() const { return command_line_w_; }
  int* argc_ptr() { return &argc_; }
  char*** argv_ptr() { return &argv_; }
  wchar_t*** wargv_ptr() { return &wargv_; }
  char* winmain_args_a() { return command_line_a_.data() + winmain_offset_a_; }
  wchar_t* winmain_args_w() { return command_line_w_.data() + winmain_offset_w_; }

  LPTOP_LEVEL_EXCEPTION_FILTER set_title_crash_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
  long crash_filter(EXCEPTION_POINTERS* info);

  // report.json: what the title called that we do not provide yet.
  void write_report() const;
  // logs\hang.txt: every other thread's name and unwound stack (when a run times out).
  void dump_threads();

 private:
  MappedImage* require_module(std::string_view name, std::string& error);  // map + bind
  bool bind(MappedImage& m, std::string& error);
  bool initialize(MappedImage& m, std::string& error);
  bool prepare(MappedImage& m, bool is_executable, std::string& error);  // CPU patches, TLS, protection
  void call_tls_callbacks(const MappedImage& m, unsigned long reason);
  HMODULE host_module(const std::string& dll);
  std::string host_ordinal_name(const std::string& dll, uint16_t ordinal);
  void build_command_line();
  std::string describe_address(uint64_t address);

  RunOptions options_;
  loader::ImportPolicy policy_;
  hle::Registry registry_;
  StubFactory stubs_;
  TlsManager tls_;

  std::recursive_mutex mutex_;
  std::vector<std::unique_ptr<MappedImage>> modules_;  // load order
  static constexpr size_t kMaxImages = 256;
  std::array<std::atomic<MappedImage*>, kMaxImages> images_{};  // the same, readable without the lock
  std::atomic<size_t> image_count_{0};
  // Set once an image is initialized: thread start and exit, which run under the loader lock,
  // read these and never take mutex_ (a thread holding it may be waiting for the loader lock).
  std::array<std::atomic<bool>, kMaxImages> ready_{};
  void publish(MappedImage* m);
  void mark_ready(const MappedImage* m);
  std::map<const MappedImage*, std::vector<MappedImage*>> deps_;
  std::map<const MappedImage*, CpuCompatResult> cpu_;
  MappedImage* exe_ = nullptr;
  bool loaded_ = false;
  std::map<std::string, HMODULE> host_modules_;
  std::map<std::string, std::map<uint16_t, std::string>> host_ordinals_;
  std::vector<std::unique_ptr<std::string>> pseudo_names_;  // a pseudo handle is the address of its name

  std::string command_line_a_;
  std::wstring command_line_w_;
  size_t winmain_offset_a_ = 0, winmain_offset_w_ = 0;
  std::vector<std::string> args_a_;
  std::vector<std::wstring> args_w_;
  std::vector<char*> argv_storage_;
  std::vector<wchar_t*> wargv_storage_;
  int argc_ = 0;
  char** argv_ = nullptr;
  wchar_t** wargv_ = nullptr;

  void* rtl_pc_to_file_header_ = nullptr;  // the host's
  void* rtl_pc_wrapper_ = nullptr;         // ours
  LPTOP_LEVEL_EXCEPTION_FILTER title_filter_ = nullptr;
  volatile long crashing_ = 0;
};

// The running title, for the wrappers.
TitleProcess* process();

}  // namespace am::rt
