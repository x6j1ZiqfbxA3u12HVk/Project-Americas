// am-inspect: read-only census of the title's executable and DLLs: build identity, image layout,
// and where every import will be bound (the rules live in loader/import_policy.h).
//
//   am-inspect [<exe|dll|folder>] [--imports] [--exports] [--no-hash] [--json <file>]
//
// Without a path it inspects game_dir from americas.ini. A folder means every .exe and .dll at
// its top level. Files are opened read-only. To check that the host has the functions the title
// wants, host DLLs are loaded from System32 only; nothing from the inspected folder is loaded
// or run.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/file_view.h"
#include "core/json.h"
#include "core/sha256.h"
#include "core/text.h"
#include "loader/import_policy.h"
#include "pe/pe_image.h"

namespace fs = std::filesystem;
using namespace am;
using am::loader::Route;

namespace {

struct Options {
  fs::path target;
  bool list_imports = false;
  bool list_exports = false;
  bool hash = true;
  fs::path json;
};

struct Module {
  fs::path path;
  std::string file_name;
  uint64_t file_size = 0;
  std::optional<Sha256> hash;
  pe::Image image;
};

// One import of one module, classified.
struct Binding {
  const Module* importer = nullptr;
  std::string dll;
  std::string function;  // name, or the host's name for an ordinal when known
  uint16_t ordinal = 0;
  bool by_ordinal = false;
  bool delay = false;
  Route route = Route::Unresolved;
  std::string_view reason;  // for Route::Wrap
  std::optional<bool> provided;  // does the providing module export it? (host and title routes)
};

std::string display_name(const Binding& b) {
  if (!b.function.empty()) return b.by_ordinal ? b.function + " (#" + std::to_string(b.ordinal) + ")" : b.function;
  return "#" + std::to_string(b.ordinal);
}

// Host DLLs, loaded from System32 only.
class HostModules {
 public:
  struct Entry {
    HMODULE module = nullptr;
    std::string error;
    bool ordinals_loaded = false;
    std::unordered_map<uint32_t, std::string> ordinal_names;
  };

  Entry& get(const std::string& dll) {
    const std::string key = to_lower_ascii(dll);
    auto [it, inserted] = entries_.try_emplace(key);
    if (inserted) {
      it->second.module = LoadLibraryExW(widen(dll).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!it->second.module) it->second.error = win32_error_text(GetLastError());
    }
    return it->second;
  }

  bool exports(const std::string& dll, const std::string& name, uint16_t ordinal, bool by_ordinal) {
    Entry& e = get(dll);
    if (!e.module) return false;
    const FARPROC p = by_ordinal && name.empty() ? GetProcAddress(e.module, MAKEINTRESOURCEA(ordinal))
                                                 : GetProcAddress(e.module, name.c_str());
    return p != nullptr;
  }

  // The name the host's module gives an ordinal (empty if unknown).
  std::string ordinal_name(const std::string& dll, uint16_t ordinal) {
    Entry& e = get(dll);
    if (!e.module) return {};
    if (!e.ordinals_loaded) {
      e.ordinals_loaded = true;
      wchar_t path[MAX_PATH * 2];
      const DWORD n = GetModuleFileNameW(e.module, path, static_cast<DWORD>(std::size(path)));
      std::string error;
      if (n > 0 && n < std::size(path)) {
        if (auto view = FileView::open(path, error)) {
          if (auto image = pe::parse(view->bytes(), error)) {
            for (const pe::Export& x : image->exports) {
              if (!x.name.empty()) e.ordinal_names[x.ordinal] = x.name;
            }
          }
        }
      }
    }
    const auto it = e.ordinal_names.find(ordinal);
    return it == e.ordinal_names.end() ? std::string() : it->second;
  }

 private:
  std::map<std::string, Entry> entries_;
};

bool has_extension(const fs::path& p, std::wstring_view ext) {
  const std::wstring e = p.extension().wstring();
  return CompareStringOrdinal(e.c_str(), static_cast<int>(e.size()), ext.data(), static_cast<int>(ext.size()), TRUE) ==
         CSTR_EQUAL;
}

int usage() {
  std::fprintf(stderr,
               "usage: am-inspect [<exe|dll|folder>] [--imports] [--exports] [--no-hash] [--json <file>]\n"
               "  without a path: game_dir from americas.ini\n");
  return 2;
}

std::optional<Options> parse_args(int argc, wchar_t** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--imports") o.list_imports = true;
    else if (a == L"--exports") o.list_exports = true;
    else if (a == L"--no-hash") o.hash = false;
    else if (a == L"--json" && i + 1 < argc) o.json = argv[++i];
    else if (!a.empty() && a[0] != L'-' && o.target.empty()) o.target = a;
    else return std::nullopt;
  }
  return o;
}

void print_module(const Module& m, const Options& o) {
  const pe::Image& img = m.image;
  std::printf("%s\n", m.file_name.c_str());
  std::printf("  file      %s", format_bytes(m.file_size).c_str());
  if (m.hash) std::printf(", sha256 %s", hex_bytes(m.hash->data(), m.hash->size()).c_str());
  std::printf("\n  image     %s, %s, %s, base 0x%s, size 0x%s, entry +0x%s\n", pe::machine_name(img.machine),
              pe::subsystem_name(img.subsystem), img.is_dll() ? "DLL" : "executable", hex(img.image_base).c_str(),
              hex(img.size_of_image).c_str(), hex(img.entry_rva).c_str());
  std::printf("  build     timestamp 0x%s", hex(img.timestamp, 8).c_str());
  if (img.codeview) {
    const std::string pdb = img.codeview->pdb_path;
    const size_t slash = pdb.find_last_of("\\/");
    std::printf(", CodeView %s age %u (%s)", pe::guid_string(img.codeview->guid).c_str(), img.codeview->age,
                (slash == std::string::npos ? pdb : pdb.substr(slash + 1)).c_str());
  }
  std::printf("\n  sections ");
  for (const pe::Section& s : img.sections) std::printf(" %s %s;", s.name.c_str(), format_bytes(s.virtual_size).c_str());
  std::printf("\n");
  if (img.tls) {
    std::printf("  TLS       template %s + %s zero fill, %zu callback(s)\n", format_bytes(img.tls->template_size()).c_str(),
                format_bytes(img.tls->zero_fill).c_str(), img.tls->callbacks.size());
  }
  std::printf("  unwind    %u functions; relocations %u blocks / %u entries\n", img.exception_entries,
              img.relocation_blocks, img.relocation_entries);
  size_t delay = 0;
  for (const pe::ImportedModule& im : img.imports) delay += im.delay_load ? 1 : 0;
  std::printf("  imports   %zu functions from %zu modules (%zu delay-load); exports %zu\n", img.import_count(),
              img.imports.size(), delay, img.exports.size());
  for (const std::string& w : img.warnings) std::printf("  warning   %s\n", w.c_str());
  if (o.list_exports) {
    for (const pe::Export& e : img.exports) {
      std::printf("    export #%-5u %s%s%s\n", e.ordinal, e.name.empty() ? "(no name)" : e.name.c_str(),
                  e.forwarder.empty() ? "" : " -> ", e.forwarder.c_str());
    }
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const auto parsed = parse_args(argc, argv);
  if (!parsed) return usage();
  Options opt = *parsed;

  std::vector<std::string> warnings;
  const Config config = load_config(warnings);
  for (const std::string& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
  if (opt.target.empty()) {
    if (config.game_dir.empty()) {
      std::fprintf(stderr, "no path given and no game_dir in %s\n", narrow(config.ini_path.wstring()).c_str());
      return usage();
    }
    opt.target = config.game_dir;
  }

  // What to inspect, and which DLLs count as the title's own.
  std::error_code ec;
  std::vector<fs::path> targets;
  fs::path title_dir;
  if (fs::is_directory(opt.target, ec)) {
    title_dir = opt.target;
    for (const auto& entry : fs::directory_iterator(title_dir, ec)) {
      if (entry.is_regular_file(ec) && (has_extension(entry.path(), L".exe") || has_extension(entry.path(), L".dll"))) {
        targets.push_back(entry.path());
      }
    }
    // The executable first, then the DLLs, each by name.
    std::sort(targets.begin(), targets.end(), [](const fs::path& a, const fs::path& b) {
      const bool ae = has_extension(a, L".exe"), be = has_extension(b, L".exe");
      if (ae != be) return ae;
      return to_lower_ascii(narrow(a.filename().wstring())) < to_lower_ascii(narrow(b.filename().wstring()));
    });
  } else {
    targets.push_back(opt.target);
    title_dir = opt.target.parent_path();
  }
  if (targets.empty()) {
    std::fprintf(stderr, "no .exe or .dll in %s\n", narrow(opt.target.wstring()).c_str());
    return 1;
  }
  std::vector<std::string> title_dlls;
  for (const auto& entry : fs::directory_iterator(title_dir, ec)) {
    if (entry.is_regular_file(ec) && has_extension(entry.path(), L".dll")) {
      title_dlls.push_back(narrow(entry.path().filename().wstring()));
    }
  }
  const loader::ImportPolicy policy(title_dlls);

  // Parse every module.
  std::vector<Module> modules;
  int failures = 0;
  for (const fs::path& p : targets) {
    std::string error;
    auto view = FileView::open(p, error);
    if (!view) {
      std::fprintf(stderr, "%s: %s\n", narrow(p.wstring()).c_str(), error.c_str());
      ++failures;
      continue;
    }
    auto image = pe::parse(view->bytes(), error);
    if (!image) {
      std::fprintf(stderr, "%s: %s\n", narrow(p.wstring()).c_str(), error.c_str());
      ++failures;
      continue;
    }
    Module m;
    m.path = p;
    m.file_name = narrow(p.filename().wstring());
    m.file_size = view->size();
    if (opt.hash) m.hash = sha256(view->bytes());
    m.image = std::move(*image);
    modules.push_back(std::move(m));
  }

  // Classify every import.
  HostModules host;
  std::vector<Binding> bindings;
  for (const Module& m : modules) {
    for (const pe::ImportedModule& im : m.image.imports) {
      const bool console = loader::is_console_only_module(im.dll);
      const bool title = policy.is_title_module(im.dll);
      const Module* title_module = nullptr;
      if (title) {
        for (const Module& other : modules) {
          if (iequals_ascii(other.file_name, im.dll)) title_module = &other;
        }
      }
      for (const pe::ImportedFunction& f : im.functions) {
        Binding b;
        b.importer = &m;
        b.dll = im.dll;
        b.function = f.name;
        b.ordinal = f.ordinal;
        b.by_ordinal = f.by_ordinal;
        b.delay = im.delay_load;
        if (f.by_ordinal && !console && !title && loader::is_host_module(im.dll)) {
          b.function = host.ordinal_name(im.dll, f.ordinal);
        }
        b.route = policy.route(im.dll, b.function);
        if (b.route == Route::Wrap) b.reason = loader::wrap_reason(b.function);
        if (b.route == Route::Host || b.route == Route::Wrap) {
          b.provided = host.exports(im.dll, b.function, f.ordinal, f.by_ordinal);
        } else if (b.route == Route::Title && title_module) {
          const auto& exports = title_module->image.exports;
          b.provided = std::any_of(exports.begin(), exports.end(), [&](const pe::Export& e) {
            return f.by_ordinal ? e.ordinal == f.ordinal : e.name == f.name;
          });
        }
        bindings.push_back(std::move(b));
      }
    }
  }

  // Report.
  for (const Module& m : modules) {
    print_module(m, opt);
    if (opt.list_imports) {
      for (const Binding& b : bindings) {
        if (b.importer != &m) continue;
        std::printf("    [%-10s] %s!%s%s%s%s\n", loader::route_name(b.route), b.dll.c_str(), display_name(b).c_str(),
                    b.delay ? " (delay-load)" : "", b.reason.empty() ? "" : ("  - " + std::string(b.reason)).c_str(),
                    b.provided == false ? "  ** not exported by the provider" : "");
      }
    }
    std::printf("\n");
  }

  std::map<Route, size_t> counts;
  for (const Binding& b : bindings) ++counts[b.route];
  std::printf("Import routes: %zu imports in %zu module(s)\n", bindings.size(), modules.size());
  const std::pair<Route, const char*> legend[] = {
      {Route::Ours, "console-only modules we implement"},
      {Route::Wrap, "host functions given console behaviour by our wrapper"},
      {Route::Host, "host functions used as they are"},
      {Route::Title, "functions of the title's own DLLs"},
      {Route::Unresolved, "nothing provides them: stubs that report the call"},
  };
  for (const auto& [route, what] : legend) {
    std::printf("  %-10s %6zu  %s\n", loader::route_name(route), counts[route], what);
  }

  // Console-only modules: what we must implement, and who needs it.
  std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> ours;  // dll -> (functions, importers)
  std::map<std::string_view, std::set<std::string>> wrapped;                            // reason -> functions
  std::vector<const Binding*> missing, unresolved;
  for (const Binding& b : bindings) {
    if (b.route == Route::Ours) {
      auto& entry = ours[to_lower_ascii(b.dll)];
      entry.first.insert(display_name(b));
      entry.second.insert(b.importer->file_name);
    } else if (b.route == Route::Wrap) {
      wrapped[b.reason].insert(b.function);
    } else if (b.route == Route::Unresolved) {
      unresolved.push_back(&b);
    }
    if (b.provided == false) missing.push_back(&b);
  }
  std::printf("\nConsole-only modules (ours)\n");
  for (const auto& [dll, entry] : ours) {
    std::string users;
    for (const std::string& u : entry.second) users += (users.empty() ? "" : ", ") + u;
    std::printf("  %-18s %3zu function(s), used by %s\n", dll.c_str(), entry.first.size(), users.c_str());
    std::string line = "     ";
    for (const std::string& f : entry.first) {
      if (line.size() + f.size() > 110) {
        std::printf("%s\n", line.c_str());
        line = "     ";
      }
      line += " " + f;
    }
    std::printf("%s\n", line.c_str());
  }
  std::printf("\nWrapped host functions\n");
  for (const auto& [reason, functions] : wrapped) {
    std::string list;
    for (const std::string& f : functions) list += (list.empty() ? "" : ", ") + f;
    std::printf("  %-24s %s\n", std::string(reason).c_str(), list.c_str());
  }
  std::printf("\nNot exported by their provider on this PC: %zu\n", missing.size());
  for (const Binding* b : missing) {
    const std::string error = (b->route == Route::Host || b->route == Route::Wrap) ? host.get(b->dll).error : std::string();
    std::printf("  %s!%s (needed by %s)%s%s\n", b->dll.c_str(), display_name(*b).c_str(), b->importer->file_name.c_str(),
                error.empty() ? "" : ": ", error.c_str());
  }
  std::printf("\nUnresolved: %zu\n", unresolved.size());
  for (const Binding* b : unresolved) {
    std::printf("  %s!%s (needed by %s)\n", b->dll.c_str(), display_name(*b).c_str(), b->importer->file_name.c_str());
  }

  if (!opt.json.empty()) {
    JsonWriter j;
    j.begin_object();
    j.field("target", narrow(opt.target.wstring()));
    j.key("modules").begin_array();
    for (const Module& m : modules) {
      const pe::Image& img = m.image;
      j.begin_object().field("file", m.file_name).field("size", m.file_size);
      j.key("sha256");
      if (m.hash) j.value(hex_bytes(m.hash->data(), m.hash->size())); else j.null();
      j.field("machine", pe::machine_name(img.machine)).field("dll", img.is_dll())
          .field("image_base", "0x" + hex(img.image_base)).field("size_of_image", img.size_of_image)
          .field("entry_rva", "0x" + hex(img.entry_rva)).field("timestamp", img.timestamp)
          .field("exception_entries", img.exception_entries).field("relocation_entries", img.relocation_entries)
          .field("exports", img.exports.size());
      if (img.codeview) {
        j.key("codeview").begin_object().field("guid", pe::guid_string(img.codeview->guid))
            .field("age", img.codeview->age).end_object();
      }
      if (img.tls) {
        j.key("tls").begin_object().field("template_size", img.tls->template_size())
            .field("zero_fill", img.tls->zero_fill).field("callbacks", img.tls->callbacks.size()).end_object();
      }
      j.key("sections").begin_array();
      for (const pe::Section& s : img.sections) {
        j.begin_object().field("name", s.name).field("rva", s.rva).field("virtual_size", s.virtual_size)
            .field("characteristics", s.characteristics).end_object();
      }
      j.end_array();
      j.key("imports").begin_array();
      for (const Binding& b : bindings) {
        if (b.importer != &m) continue;
        j.begin_object().field("dll", b.dll).field("function", b.function);
        if (b.by_ordinal) j.field("ordinal", b.ordinal);
        if (b.delay) j.field("delay_load", true);
        j.field("route", loader::route_name(b.route));
        if (!b.reason.empty()) j.field("reason", b.reason);
        if (b.provided) j.field("provided", *b.provided);
        j.end_object();
      }
      j.end_array();
      j.key("warnings").begin_array();
      for (const std::string& w : img.warnings) j.value(w);
      j.end_array();
      j.end_object();
    }
    j.end_array();
    j.key("summary").begin_object();
    for (const auto& [route, what] : legend) j.field(loader::route_name(route), counts[route]);
    j.end_object();
    j.end_object();
    std::ofstream out(opt.json, std::ios::binary);
    out << j.str() << '\n';
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", narrow(opt.json.wstring()).c_str());
      return 1;
    }
    std::printf("\nwrote %s\n", narrow(opt.json.wstring()).c_str());
  }
  return failures ? 1 : 0;
}
