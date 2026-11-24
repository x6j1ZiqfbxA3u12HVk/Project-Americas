#include "runtime/process.h"

#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "core/json.h"
#include "core/text.h"
#include "runtime/console.h"
#include "runtime/d3d12x/object.h"
#include "runtime/gdk/runtime.h"
#include "runtime/host_patch.h"
#include "runtime/launcher_api.h"
#include "runtime/log.h"
#include "runtime/memory/title_memory.h"

namespace am::rt {
namespace {

TitleProcess* g_process = nullptr;

std::string normalize_module_name(std::string_view name) {
  std::string n(name);
  const size_t slash = n.find_last_of("\\/");
  if (slash != std::string::npos) n = n.substr(slash + 1);
  if (n.find('.') == std::string::npos) n += ".dll";
  return to_lower_ascii(n);
}

std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
    else out.append(backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  out += L'"';
  return out;
}

LONG WINAPI top_level_filter(EXCEPTION_POINTERS* info) { return g_process ? g_process->crash_filter(info) : 0; }

}  // namespace

TitleProcess* process() { return g_process; }

TitleProcess::TitleProcess(RunOptions options) : options_(std::move(options)), stubs_(options_.unimplemented) {
  g_process = this;
}

// ---- command line ----

void TitleProcess::build_command_line() {
  args_w_.clear();
  args_w_.push_back(options_.title.wstring());
  for (const std::wstring& a : options_.args) args_w_.push_back(a);
  command_line_w_.clear();
  for (size_t i = 0; i < args_w_.size(); ++i) {
    if (i) command_line_w_ += L' ';
    command_line_w_ += quote_arg(args_w_[i]);
    if (i == 0) winmain_offset_w_ = command_line_w_.size() + (args_w_.size() > 1 ? 1 : 0);
  }
  command_line_a_ = narrow(command_line_w_);
  winmain_offset_a_ = narrow(command_line_w_.substr(0, winmain_offset_w_)).size();

  args_a_.clear();
  for (const std::wstring& a : args_w_) args_a_.push_back(narrow(a));
  argv_storage_.clear();
  wargv_storage_.clear();
  for (std::string& a : args_a_) argv_storage_.push_back(a.data());
  for (std::wstring& a : args_w_) wargv_storage_.push_back(a.data());
  argv_storage_.push_back(nullptr);
  wargv_storage_.push_back(nullptr);
  argc_ = static_cast<int>(args_a_.size());
  argv_ = argv_storage_.data();
  wargv_ = wargv_storage_.data();
}

// ---- loading ----

bool TitleProcess::load(std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::filesystem::path dir = options_.title.parent_path();
  std::vector<std::string> title_dlls;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::wstring ext = entry.path().extension().wstring();
    if (entry.is_regular_file(ec) && CompareStringOrdinal(ext.c_str(), -1, L".dll", -1, TRUE) == CSTR_EQUAL) {
      title_dlls.push_back(narrow(entry.path().filename().wstring()));
    }
  }
  policy_ = loader::ImportPolicy(title_dlls);
  build_command_line();
  SetUnhandledExceptionFilter(&top_level_filter);
  install_cpu_emulation();
  if (!tls_.discover(error)) return false;
  // Host DLLs (a host C++ runtime, say) asking which image a title address belongs to get our answer.
  if (const hle::Function* f = registry_.find_wrapper("RtlPcToFileHeader")) {
    rtl_pc_to_file_header_ = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlPcToFileHeader"));
    rtl_pc_wrapper_ = f->address;
    logf("host import tables redirected for RtlPcToFileHeader: %zu", patch_loaded_imports(rtl_pc_to_file_header_, rtl_pc_wrapper_));
  }

  MapOptions map_options;
  map_options.force_relocation = options_.force_relocation;
  if (options_.image_window) {
    VirtualFree(options_.image_window, 0, MEM_RELEASE);
    options_.image_window = nullptr;
  } else {
    logf("the launcher could not hold 0x%s: the executable may have to move", hex(kImageWindow).c_str());
  }
  auto exe = map_image(options_.title, map_options, error);
  if (!exe) return false;
  if (exe->info.is_dll()) {
    error = narrow(options_.title.wstring()) + " is a DLL, not an executable";
    return false;
  }
  modules_.push_back(std::make_unique<MappedImage>(std::move(*exe)));
  exe_ = modules_.back().get();
  publish(exe_);
  logf("mapped %s at %p%s (%s)", exe_->name.c_str(), static_cast<void*>(exe_->base), exe_->relocated ? ", relocated" : "",
       format_bytes(exe_->size).c_str());
  if (!bind(*exe_, error)) return false;
  if (!prepare(*exe_, true, error)) return false;
  for (MappedImage* dep : deps_[exe_]) {
    if (!initialize(*dep, error)) return false;
  }
  return true;
}

MappedImage* TitleProcess::require_module(std::string_view name, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (MappedImage* m = find_module(name)) return m;
  if (!policy_.is_title_module(normalize_module_name(name))) return nullptr;

  std::filesystem::path path;
  const std::string wanted = normalize_module_name(name);
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(options_.title.parent_path(), ec)) {
    if (to_lower_ascii(narrow(entry.path().filename().wstring())) == wanted) path = entry.path();
  }
  if (path.empty()) {
    error = "title module " + wanted + " not found";
    return nullptr;
  }
  MapOptions map_options;
  map_options.force_relocation = options_.force_relocation;
  auto mapped = map_image(path, map_options, error);
  if (!mapped) return nullptr;
  if (!mapped->info.is_dll()) {
    error = mapped->name + " is not a DLL";
    return nullptr;
  }
  modules_.push_back(std::make_unique<MappedImage>(std::move(*mapped)));
  MappedImage* m = modules_.back().get();
  publish(m);
  logf("mapped %s at %p%s (%s)", m->name.c_str(), static_cast<void*>(m->base), m->relocated ? ", relocated" : "",
       format_bytes(m->size).c_str());
  if (!bind(*m, error) || !prepare(*m, false, error)) {
    error = m->name + ": " + error;
    return nullptr;
  }
  return m;
}

MappedImage* TitleProcess::load_module(std::string_view name, std::string& error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  MappedImage* m = require_module(name, error);
  if (!m) return nullptr;
  if (!initialize(*m, error)) return nullptr;
  return m;
}

bool TitleProcess::bind(MappedImage& m, std::string& error) {
  for (const pe::ImportedModule& im : m.info.imports) {
    if (!im.iat_rva) continue;
    auto* iat = m.at<uint64_t>(im.iat_rva);
    if (!m.contains(iat) || !m.contains(iat + im.functions.size())) {
      error = "import table of " + im.dll + " lies outside the image";
      return false;
    }
    for (size_t k = 0; k < im.functions.size(); ++k) {
      const pe::ImportedFunction& f = im.functions[k];
      void* address = resolve_import(im.dll, f.name, f.ordinal, f.by_ordinal);
      if (!address) {
        error = "cannot bind " + im.dll + "!" + (f.by_ordinal ? "#" + std::to_string(f.ordinal) : f.name);
        return false;
      }
      iat[k] = reinterpret_cast<uint64_t>(address);
    }
    if (im.delay_load && im.module_handle_rva) {
      // Pre-bound: give the delay-load helper a handle so it never runs for this module.
      HMODULE handle = nullptr;
      if (MappedImage* t = find_module(im.dll)) handle = reinterpret_cast<HMODULE>(t->base);
      else if (!loader::is_console_only_module(im.dll)) handle = host_module(im.dll);
      *m.at<uint64_t>(im.module_handle_rva) = reinterpret_cast<uint64_t>(handle);
    }
    if (MappedImage* t = find_module(im.dll); t && t != &m) deps_[&m].push_back(t);
  }
  return true;
}

bool TitleProcess::prepare(MappedImage& m, bool is_executable, std::string& error) {
  CpuCompatResult cpu;
  if (!apply_cpu_compat(m, options_.cache_dir, options_.allow_missing_cpu_features, cpu, error)) return false;
  cpu_[&m] = cpu;
  return tls_.install(m, is_executable, error) && finish_image(m, error);
}

void TitleProcess::call_tls_callbacks(const MappedImage& m, unsigned long reason) {
  if (!m.info.tls) return;
  const int64_t delta = reinterpret_cast<int64_t>(m.base) - static_cast<int64_t>(m.info.image_base);
  for (uint64_t va : m.info.tls->callbacks) {
    auto cb = reinterpret_cast<PIMAGE_TLS_CALLBACK>(static_cast<int64_t>(va) + delta);
    cb(m.base, reason, nullptr);
  }
}

bool TitleProcess::initialize(MappedImage& m, std::string& error) {
  if (m.initialized || &m == exe_) return true;
  m.initialized = true;  // set first: dependency cycles end here
  for (MappedImage* dep : deps_[&m]) {
    if (!initialize(*dep, error)) return false;
  }
  call_tls_callbacks(m, DLL_PROCESS_ATTACH);
  if (m.info.entry_rva) {
    using DllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    auto entry = reinterpret_cast<DllMain>(m.base + m.info.entry_rva);
    if (!entry(reinterpret_cast<HINSTANCE>(m.base), DLL_PROCESS_ATTACH, nullptr)) {
      error = m.name + ": DllMain refused to initialize";
      return false;
    }
  }
  mark_ready(&m);
  logf("initialized %s", m.name.c_str());
  return true;
}

int TitleProcess::run() {
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    loaded_ = true;
  }
  call_tls_callbacks(*exe_, DLL_PROCESS_ATTACH);
  exe_->initialized = true;
  mark_ready(exe_);
  using Entry = int(__cdecl*)(void*);
  auto entry = reinterpret_cast<Entry>(exe_->base + exe_->info.entry_rva);
  logf("starting %s", exe_->name.c_str());
  return entry(reinterpret_cast<void*>(__readgsqword(0x60)));  // the entry point gets the PEB, as from Windows
}

void TitleProcess::on_thread(unsigned long reason) {
  const size_t n = image_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (!ready_[i].load(std::memory_order_acquire)) continue;
    MappedImage* m = images_[i].load(std::memory_order_acquire);
    call_tls_callbacks(*m, reason);
    if (m != exe_ && m->thread_calls && m->info.entry_rva) {
      using DllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
      reinterpret_cast<DllMain>(m->base + m->info.entry_rva)(reinterpret_cast<HINSTANCE>(m->base), reason, nullptr);
    }
  }
}

// ---- lookups ----

MappedImage* TitleProcess::find_module(std::string_view name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string wanted = normalize_module_name(name);
  for (const auto& m : modules_) {
    if (to_lower_ascii(m->name) == wanted) return m.get();
  }
  return nullptr;
}

MappedImage* TitleProcess::module_containing(const void* address) {
  const size_t n = image_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    MappedImage* m = images_[i].load(std::memory_order_acquire);
    if (m && m->contains(address)) return m;
  }
  return nullptr;
}

void TitleProcess::mark_ready(const MappedImage* m) {
  const size_t n = image_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (images_[i].load(std::memory_order_acquire) == m) ready_[i].store(true, std::memory_order_release);
  }
}

void TitleProcess::publish(MappedImage* m) {
  const size_t n = image_count_.load(std::memory_order_relaxed);
  if (n >= kMaxImages) return;
  images_[n].store(m, std::memory_order_release);
  image_count_.store(n + 1, std::memory_order_release);
}

HMODULE TitleProcess::pseudo_module(std::string_view name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string n = normalize_module_name(name);
  if (!loader::is_console_only_module(n)) return nullptr;
  for (const auto& p : pseudo_names_) {
    if (*p == n) return reinterpret_cast<HMODULE>(p.get());
  }
  pseudo_names_.push_back(std::make_unique<std::string>(n));
  return reinterpret_cast<HMODULE>(pseudo_names_.back().get());
}

const std::string* TitleProcess::pseudo_module_name(HMODULE handle) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& p : pseudo_names_) {
    if (reinterpret_cast<HMODULE>(p.get()) == handle) return p.get();
  }
  return nullptr;
}

HMODULE TitleProcess::host_module(const std::string& dll) {
  const std::string key = to_lower_ascii(dll);
  auto it = host_modules_.find(key);
  if (it != host_modules_.end()) return it->second;
  HMODULE h = LoadLibraryExW(widen(dll).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  host_modules_[key] = h;
  if (h && rtl_pc_wrapper_) patch_module_imports(h, rtl_pc_to_file_header_, rtl_pc_wrapper_);
  return h;
}

std::string TitleProcess::host_ordinal_name(const std::string& dll, uint16_t ordinal) {
  const std::string key = to_lower_ascii(dll);
  auto [it, inserted] = host_ordinals_.try_emplace(key);
  if (inserted) {
    if (HMODULE h = host_module(dll)) {
      const auto* base = reinterpret_cast<const uint8_t*>(h);
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
      const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
      if (d.VirtualAddress) {
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress);
        const auto* names = reinterpret_cast<const uint32_t*>(base + ed->AddressOfNames);
        const auto* ords = reinterpret_cast<const uint16_t*>(base + ed->AddressOfNameOrdinals);
        for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
          it->second[static_cast<uint16_t>(ed->Base + ords[i])] = reinterpret_cast<const char*>(base + names[i]);
        }
      }
    }
  }
  const auto found = it->second.find(ordinal);
  return found == it->second.end() ? std::string() : found->second;
}

void* TitleProcess::resolve_import(std::string_view dll_view, std::string_view function, uint16_t ordinal,
                                   bool by_ordinal) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string dll(dll_view);
  std::string fn(function);
  const bool console = loader::is_console_only_module(dll);
  const bool title = policy_.is_title_module(dll);
  if (by_ordinal && fn.empty() && !console && !title && loader::is_host_module(dll)) fn = host_ordinal_name(dll, ordinal);
  const std::string label = fn.empty() ? "#" + std::to_string(ordinal) : fn;

  switch (policy_.route(dll, fn)) {
    case loader::Route::Ours:
      // By name, or as "#<ordinal>" for functions a console module exports by ordinal only.
      if (const hle::Function* f = registry_.find(dll, fn.empty() ? label : fn)) return f->address;
      return stubs_.stub(dll, label, "console function not implemented yet");
    case loader::Route::Wrap:
      if (const hle::Function* f = registry_.find_wrapper(fn)) return f->address;
      return stubs_.stub(dll, label, "needs console behaviour we do not provide yet; refused");
    case loader::Route::Host:
      if (HMODULE h = host_module(dll)) {
        const FARPROC p = (by_ordinal && fn.empty()) ? GetProcAddress(h, MAKEINTRESOURCEA(ordinal))
                                                     : GetProcAddress(h, fn.c_str());
        if (p) return reinterpret_cast<void*>(p);
      }
      return stubs_.stub(dll, label, "not on this PC");
    case loader::Route::Title: {
      std::string error;
      MappedImage* t = loaded_ ? load_module(dll, error) : require_module(dll, error);
      if (!t) return stubs_.stub(dll, label, "title module did not load: " + error);
      const MappedImage::Export e = by_ordinal ? t->find_export_ordinal(ordinal) : t->find_export(fn);
      if (void* const* p = std::get_if<void*>(&e)) return *p;
      if (const std::string* fwd = std::get_if<std::string>(&e)) {
        const size_t dot = fwd->find('.');
        if (dot != std::string::npos) {
          const std::string mod = fwd->substr(0, dot) + ".dll";
          const std::string target = fwd->substr(dot + 1);
          if (!target.empty() && target[0] == '#') {
            return resolve_import(mod, "", static_cast<uint16_t>(std::atoi(target.c_str() + 1)), true);
          }
          return resolve_import(mod, target, 0, false);
        }
      }
      return stubs_.stub(dll, label, "the title module does not export it");
    }
    case loader::Route::Unresolved:
      break;
  }
  return stubs_.stub(dll, label, "nothing provides it");
}

// ---- crashes ----

LPTOP_LEVEL_EXCEPTION_FILTER TitleProcess::set_title_crash_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  LPTOP_LEVEL_EXCEPTION_FILTER old = title_filter_;
  title_filter_ = filter;
  return old;
}

std::string TitleProcess::describe_address(uint64_t address) {
  if (const MappedImage* m = module_containing(reinterpret_cast<const void*>(address))) {
    return m->name + "+0x" + hex(address - reinterpret_cast<uint64_t>(m->base));
  }
  HMODULE h = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &h)) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(h, path, MAX_PATH);
    return narrow(std::filesystem::path(path).filename().wstring()) + "+0x" + hex(address - reinterpret_cast<uint64_t>(h));
  }
  return "0x" + hex(address);
}

long TitleProcess::crash_filter(EXCEPTION_POINTERS* info) {
  if (InterlockedIncrement(&crashing_) == 1) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    CONTEXT ctx = *info->ContextRecord;
    std::string text = "crash: exception 0x" + hex(r->ExceptionCode, 8) + " at " +
                       describe_address(reinterpret_cast<uint64_t>(r->ExceptionAddress));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
      text += r->ExceptionInformation[0] == 0 ? ", reading 0x" : r->ExceptionInformation[0] == 1 ? ", writing 0x" : ", executing 0x";
      text += hex(r->ExceptionInformation[1]);
    }
    if (r->ExceptionCode == kUnimplementedException && r->NumberParameters >= 1) {
      const auto* stub = reinterpret_cast<const StubInfo*>(r->ExceptionInformation[0]);
      text += ": unimplemented " + stub->dll + "!" + stub->function;
    }
    char regs[512];
    std::snprintf(regs, sizeof regs,
                  "\n  rax %016llx rbx %016llx rcx %016llx rdx %016llx\n  rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n"
                  "  r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n  r12 %016llx r13 %016llx r14 %016llx r15 %016llx",
                  ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp, ctx.R8, ctx.R9, ctx.R10,
                  ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);
    text += regs;
    text += "\n  stack:";
    for (int frame = 0; frame < 32 && ctx.Rip; ++frame) {
      text += "\n    " + describe_address(ctx.Rip);
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
      if (fn) {
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
      } else {
        if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      }
    }
    log_line(text);
    std::ofstream(options_.logs_dir / L"crash.txt", std::ios::binary) << text << '\n';
    write_report();
  }
  if (title_filter_) return title_filter_(info);
  return EXCEPTION_EXECUTE_HANDLER;
}

void TitleProcess::dump_threads() {
  struct Captured {
    DWORD id;
    uint64_t frames[40];
    int count;
  };
  std::vector<DWORD> ids;
  const DWORD self = GetCurrentThreadId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te)) {
      if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
    }
    CloseHandle(snapshot);
  }
  std::vector<Captured> captured(ids.size());
  std::string text = "threads: " + std::to_string(ids.size()) + " besides the reporting one\n";
  for (size_t i = 0; i < ids.size(); ++i) {
    Captured& c = captured[i];
    c.id = ids[i];
    c.count = 0;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, c.id);
    if (!t) continue;
    // Nothing that allocates while the thread is stopped: it may hold the heap's lock.
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &ctx)) {
        for (; c.count < 40 && ctx.Rip; ++c.count) {
          c.frames[c.count] = ctx.Rip;
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
          if (!fn) {
            if (c.count) break;
            // A leaf function (a system call stub): its return address is on top of the stack.
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
          }
          void* handler_data = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
        }
      }
      ResumeThread(t);
    }
    PWSTR name = nullptr;
    std::string label;
    if (SUCCEEDED(GetThreadDescription(t, &name)) && name) {
      label = narrow(name);
      LocalFree(name);
    }
    CloseHandle(t);
    text += "\nthread " + std::to_string(c.id) + (label.empty() ? "" : " \"" + label + "\"") + ":";
    for (int k = 0; k < c.count; ++k) text += "\n    " + describe_address(c.frames[k]);
  }
  std::ofstream(options_.logs_dir / L"hang.txt", std::ios::binary) << text << '\n';
  logf("hang.txt: stacks of %zu threads", ids.size());
}

void TitleProcess::write_report() const {
  JsonWriter j;
  j.begin_object();
  j.field("title", narrow(options_.title.wstring()));
  j.key("modules").begin_array();
  for (const auto& m : modules_) {
    j.begin_object().field("name", m->name).field("base", "0x" + hex(reinterpret_cast<uint64_t>(m->base)))
        .field("relocated", m->relocated).field("initialized", m->initialized).end_object();
  }
  j.end_array();
  j.key("stubs").begin_array();
  for (const StubInfo* s : stubs_.all()) {
    j.begin_object().field("import", s->dll + "!" + s->function).field("why", s->why).field("calls", s->calls).end_object();
  }
  j.end_array();
  j.key("cpu").begin_object();
  for (const auto& [m, r] : cpu_) {
    j.key(m->name).begin_object().field("patched", r.patched).field("emulated_sites", r.emulated)
        .field("census_from_cache", r.from_cache).end_object();
  }
  j.key("emulation_hits").begin_array();
  for (const EmulationHit& h : emulation_hits()) {
    j.begin_object().field("at", const_cast<TitleProcess*>(this)->describe_address(h.address)).field("count", h.count).end_object();
  }
  j.end_array();
  j.end_object();
  const mem::MemoryStats ms = mem::title_memory().stats();
  j.key("memory").begin_object();
  j.field("console", std::string(current_console().name)).field("physical_capacity", ms.physical_capacity)
      .field("physical_allocated", ms.physical_allocated).field("physical_peak", ms.physical_peak)
      .field("physical_committed", ms.physical_committed).field("views", ms.views).field("map_calls", ms.map_calls)
      .field("reservations", ms.reservations).field("regions", ms.regions).field("refused", ms.refused);
  j.end_object();
  j.key("dstorage");
  storage::write_storage_report(j);
  j.key("runtime");
  gdk::write_runtime_report(j);
  d3d12x::write_graphics_report(j);
  j.key("passthrough").begin_array();
  for (const hle::Function& f : registry_.all()) {
    if (f.passthrough) j.value(f.name);
  }
  j.end_array();
  j.end_object();
  std::ofstream(options_.logs_dir / L"report.json", std::ios::binary) << j.str() << '\n';
}

}  // namespace am::rt
