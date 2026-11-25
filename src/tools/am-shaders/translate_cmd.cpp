// am-shaders translate / tsurvey: the shader translator on the corpus.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <mutex>
#include <thread>
#include <vector>

#include "core/file_view.h"
#include "core/json.h"
#include "core/text.h"
#include "host/dxc_compile.h"
#include "shader/console_program.h"
#include "shader/container.h"
#include "shader/rdna2.h"
#include "shader/stage_io.h"
#include "shader/translate.h"
#include "survey.h"

namespace fs = std::filesystem;

namespace am::tools {
namespace {

std::vector<fs::path> containers(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() == L".dxbc") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

struct Loaded {
  std::vector<uint8_t> bytes;  // the container
  shader::ConsoleShader shader;
};

std::optional<Loaded> load(const fs::path& file, std::string& error) {
  auto view = FileView::open(file, error);
  if (!view) return std::nullopt;
  Loaded out;
  out.bytes.assign(view->bytes().begin(), view->bytes().end());
  const auto c = shader::parse_container(out.bytes, error);
  const shader::Part* dxil = c ? c->find("DXIL") : nullptr;
  if (!dxil) {
    if (error.empty()) error = "no DXIL part";
    return std::nullopt;
  }
  auto s = shader::read_console_shader(dxil->data, error);
  if (!s) return std::nullopt;
  out.shader = std::move(*s);
  return out;
}

// The translator's input for a program; vertex and pixel shaders also get their interface.
// A failure to read it leaves the interface empty, and `io_error` says why.
shader::TranslateInput input_of(const std::vector<uint8_t>& container, const shader::NativeBlob& b,
                                const shader::NativeProgram& p, std::string* io_error = nullptr) {
  shader::TranslateInput in;
  if (p.stage == shader::HwStage::Vs || p.stage == shader::HwStage::Ps) {
    std::string why;
    if (!shader::read_stage_io(container, b, p, in, why) && io_error) *io_error = why;
  }
  const auto bytes = b.code(p);
  in.code = {reinterpret_cast<const uint32_t*>(bytes.data()), bytes.size() / 4};
  in.stage = p.stage;
  in.wave32 = p.wave32;
  in.vgprs = p.vgprs;
  in.lds_bytes = p.lds_bytes;
  in.cs_rsrc2 = p.cs_rsrc2;
  for (int k = 0; k < 3; ++k) in.cs_threads[k] = p.cs_threads[k];
  return in;
}

}  // namespace

int translate(const std::string& wanted, const fs::path& pack_dir, const fs::path& out_dir) {
  fs::path file;
  // A container file given by its path (one saved at run time, under cache/shaders/runtime), or
  // the pack's by hash prefix.
  if (std::error_code fe; wanted.find_first_of("\\/") != std::string::npos && fs::is_regular_file(widen(wanted), fe)) {
    file = widen(wanted);
  }
  for (const fs::path& f : file.empty() ? containers(pack_dir) : std::vector<fs::path>{}) {
    if (istarts_with_ascii(narrow(f.filename().wstring()), wanted)) {
      file = f;
      break;
    }
  }
  std::string error;
  const auto shader = file.empty() ? std::nullopt : load(file, error);
  if (!shader) {
    std::fprintf(stderr, "%s: %s\n", wanted.c_str(), file.empty() ? "no such container" : error.c_str());
    return 1;
  }
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  int index = 0, failed = 0;
  for (const shader::NativeBlob& b : shader->shader.blobs) {
    for (const shader::NativeProgram& p : b.programs) {
      std::string io_error;
      const shader::TranslateInput in = input_of(shader->bytes, b, p, &io_error);
      if (!io_error.empty()) std::printf("  interface: %s\n", io_error.c_str());
      const shader::Translation t = shader::translate(in, shader::Binding::Test);
      const std::string stem = narrow(file.stem().wstring()) + "_" + std::to_string(index++);
      if (!t.ok) {
        std::printf("%s: %s: not translated: %s\n", stem.c_str(), shader::hw_stage_name(p.stage), t.failure.c_str());
        for (const auto& [k, n] : t.unsupported) std::printf("    %4d  %s\n", n, k.c_str());
        ++failed;
        continue;
      }
      const fs::path hlsl = out_dir / widen(stem + ".hlsl");
      std::ofstream(hlsl, std::ios::binary) << t.hlsl;
      const host::CompileResult r = host::compile_hlsl(t.hlsl, t.entry, t.profile);
      std::printf("%s: %s wave%u, %zu bytes of HLSL -> %s (%s)\n", stem.c_str(), shader::hw_stage_name(p.stage), t.wave_size,
                  t.hlsl.size(), r.ok ? "DXIL" : "compile failed", narrow(hlsl.wstring()).c_str());
      if (!r.ok || !r.messages.empty()) std::printf("%s\n", r.messages.substr(0, 4000).c_str());
      failed += !r.ok;
    }
  }
  return failed ? 1 : 0;
}

int translation_survey(const fs::path& pack_dir, const fs::path& out_json, size_t compile_sample, const fs::path& hlsl_dir) {
  const auto files = containers(pack_dir);
  const auto start = std::chrono::steady_clock::now();
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  std::mutex lock;
  std::map<std::string, uint64_t> missing, stages_total, stages_ok, stages_structured;  // missing: programs blocked by it
  std::map<std::string, uint64_t> first_missing;
  std::vector<std::pair<std::string, shader::Translation>> to_compile;
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w) {
    workers.emplace_back([&] {
      for (size_t i; (i = next.fetch_add(1)) < files.size();) {
        std::string error;
        const auto shader = load(files[i], error);
        if (!shader) continue;
        for (const shader::NativeBlob& b : shader->shader.blobs) {
          for (const shader::NativeProgram& p : b.programs) {
            shader::Translation t = shader::translate(input_of(shader->bytes, b, p), shader::Binding::Test);
            std::lock_guard<std::mutex> g(lock);
            ++stages_total[shader::hw_stage_name(p.stage)];
            if (t.ok) {
              ++stages_ok[shader::hw_stage_name(p.stage)];
              if (t.structured) ++stages_structured[shader::hw_stage_name(p.stage)];
              if (to_compile.size() < compile_sample) to_compile.emplace_back(narrow(files[i].stem().wstring()), std::move(t));
            } else {
              for (const auto& [k, n] : t.unsupported) ++missing[k];
              ++first_missing[t.failure];
            }
          }
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  // With --raw, the sample's HLSL is written out (sorted by size) instead of compiled here.
  if (!hlsl_dir.empty()) {
    std::error_code ec;
    fs::create_directories(hlsl_dir, ec);
    std::map<std::string, int> seen;
    for (const auto& [name, t] : to_compile) {
      const std::string stem = name.substr(0, 16) + "_" + std::to_string(seen[name]++);
      // An unchanged file is left alone, so compiles of it (dxc_corpus.py) stay current.
      const fs::path file = hlsl_dir / widen(stem + ".hlsl");
      std::ifstream in(file, std::ios::binary);
      const std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      if (old != t.hlsl) std::ofstream(file, std::ios::binary) << t.hlsl;
    }
    std::printf("wrote %zu programs' HLSL to %s\n", to_compile.size(), narrow(hlsl_dir.wstring()).c_str());
    to_compile.clear();
  }
  // DXC on a sample of what translated, in parallel.
  std::atomic<size_t> ci{0};
  std::atomic<uint64_t> compiled{0};
  std::map<std::string, std::string> compile_errors;
  std::vector<std::pair<double, std::string>> slowest;
  size_t done_compiles = 0;
  workers.clear();
  for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w) {
    workers.emplace_back([&] {
      for (size_t i; (i = ci.fetch_add(1)) < to_compile.size();) {
        const auto& [name, t] = to_compile[i];
        const auto t0 = std::chrono::steady_clock::now();
        const host::CompileResult r = host::compile_hlsl(t.hlsl, t.entry, t.profile);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
          std::lock_guard<std::mutex> g(lock);
          slowest.emplace_back(took, name + " (" + std::to_string(t.hlsl.size() / 1024) + " KiB of HLSL)");
          if (++done_compiles % 50 == 0) std::fprintf(stderr, "  compiled %zu of %zu\n", done_compiles, to_compile.size());
        }
        if (r.ok) ++compiled;
        else {
          std::lock_guard<std::mutex> g(lock);
          if (compile_errors.size() < 20) compile_errors[name] = r.messages.substr(0, 600);
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  JsonWriter j;
  j.begin_object().field("translator_version", static_cast<uint64_t>(shader::kTranslatorVersion)).field("seconds", seconds);
  j.key("programs").begin_object();
  for (const auto& [k, n] : stages_total) {
    j.key(k).begin_object().field("total", n).field("translated", stages_ok[k]).field("structured", stages_structured[k]).end_object();
  }
  j.end_object();
  j.field("compile_sample", static_cast<uint64_t>(to_compile.size())).field("compiled", compiled.load());
  std::vector<std::pair<uint64_t, std::string>> m;
  for (const auto& [k, n] : missing) m.emplace_back(n, k);
  std::sort(m.rbegin(), m.rend());
  j.key("blocking").begin_object();
  for (const auto& [n, k] : m) j.field(k, n);
  j.end_object();
  j.key("compile_errors").begin_object();
  for (const auto& [k, v] : compile_errors) j.field(k, v);
  j.end_object();
  j.end_object();
  std::ofstream(out_json, std::ios::binary) << j.str() << '\n';

  std::printf("translation survey (translator %u), %.1f s\n", shader::kTranslatorVersion, seconds);
  for (const auto& [k, n] : stages_total) {
    std::printf("  %-3s %6llu programs, %6llu translated (%.1f %%), %llu structured\n", k.c_str(),
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(stages_ok[k]), 100.0 * stages_ok[k] / n,
                static_cast<unsigned long long>(stages_structured[k]));
  }
  std::printf("DXC: %llu of %zu compiled\n", static_cast<unsigned long long>(compiled.load()), to_compile.size());
  std::sort(slowest.rbegin(), slowest.rend());
  for (size_t i = 0; i < slowest.size() && i < 8; ++i) std::printf("  %7.1f s  %s\n", slowest[i].first, slowest[i].second.c_str());
  std::printf("most blocking (programs held up by each):\n");
  for (size_t i = 0; i < m.size() && i < 25; ++i) std::printf("  %7llu  %s\n", static_cast<unsigned long long>(m[i].first), m[i].second.c_str());
  std::printf("wrote %s\n", narrow(out_json.wstring()).c_str());
  return 0;
}

// Every image instruction of the corpus by its form (dimension, channels, D16/A16, NSA,
// 128-bit T#, TFE/LWE) and stage: what the image translation has to cover.
int image_survey(const fs::path& pack_dir) {
  namespace rd = shader::rdna2;
  const auto files = containers(pack_dir);
  std::mutex lock;
  std::map<std::string, uint64_t> forms, names, cs_names;
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w) {
    workers.emplace_back([&] {
      std::map<std::string, uint64_t> f, n, cn;
      for (size_t i; (i = next.fetch_add(1)) < files.size();) {
        std::string error;
        const auto shader = load(files[i], error);
        if (!shader) continue;
        for (const shader::NativeBlob& b : shader->shader.blobs) {
          for (const shader::NativeProgram& p : b.programs) {
            const shader::TranslateInput in = input_of(shader->bytes, b, p);
            const rd::ProgramScan scan = rd::scan_program(in.code);
            const bool cs = p.stage == shader::HwStage::Cs;
            for (const uint32_t at : scan.starts) {
              rd::Instruction insn;
              if (rd::decode(in.code, at, insn) != rd::DecodeStatus::Ok || insn.encoding != rd::Encoding::Mimg) continue;
              const std::string name = insn.info->name;
              char key[160];
              std::snprintf(key, sizeof key, "%-3s %-24s dim%u dmask%X%s%s%s%s%s%s%s", shader::hw_stage_name(p.stage), name.c_str(),
                            insn.field(rd::Field::DIM), insn.field(rd::Field::DMASK), insn.field(rd::Field::D16) ? " d16" : "",
                            insn.field(rd::Field::A16) ? " a16" : "", insn.field(rd::Field::NSA) ? " nsa" : "",
                            insn.field(rd::Field::R128) ? " r128" : "", insn.field(rd::Field::UNORM) ? " unorm" : "",
                            insn.field(rd::Field::TFE) ? " tfe" : "", insn.field(rd::Field::LWE) ? " lwe" : "");
              ++f[key];
              ++n[name];
              if (cs) ++cn[name];
            }
          }
        }
      }
      std::lock_guard<std::mutex> g(lock);
      for (const auto& [k, v] : f) forms[k] += v;
      for (const auto& [k, v] : n) names[k] += v;
      for (const auto& [k, v] : cn) cs_names[k] += v;
    });
  }
  for (auto& w : workers) w.join();
  const auto top = [](const std::map<std::string, uint64_t>& m, size_t count, const char* title) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (const auto& [k, n] : m) v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    std::printf("%s (%zu kinds)\n", title, v.size());
    for (size_t i = 0; i < v.size() && i < count; ++i) std::printf("  %9llu  %s\n", static_cast<unsigned long long>(v[i].first), v[i].second.c_str());
  };
  top(names, 60, "image instructions, all stages");
  top(cs_names, 40, "image instructions, compute");
  top(forms, 1000, "forms");
  return 0;
}

// The 24-byte marker a console program starts with, as hex: the runtime's key for it.
std::string marker_of(std::span<const uint8_t> code) {
  static const char* const kHex = "0123456789abcdef";
  std::string h;
  for (size_t i = 0; i < 24 && i < code.size(); ++i) {
    h += kHex[code[i] >> 4];
    h += kHex[code[i] & 15];
  }
  return h;
}

int precompile(const fs::path& pack_dir, const fs::path& runtime_dir, const fs::path& out_dir, bool all, const fs::path& hlsl_dir) {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // The programs the game has used: the GPU programs of its console pipelines (slots 57 and 67,
  // saved as .cpipe bundles), by marker. Bundles read before are listed in cpipe-index.tsv (the
  // bundle's name, a tab, its markers), so a pass reads only the new ones.
  std::set<std::string> wanted, indexed;
  std::vector<fs::path> files;
  {
    std::ifstream in(out_dir / L"cpipe-index.tsv");
    for (std::string line; std::getline(in, line);) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      indexed.insert(line.substr(0, tab));
      for (size_t at = tab + 1; at < line.size();) {
        const size_t comma = std::min(line.find(',', at), line.size());
        if (comma > at) wanted.insert(line.substr(at, comma - at));
        at = comma + 1;
      }
    }
  }
  size_t new_bundles = 0;
  {
    std::ofstream index(out_dir / L"cpipe-index.tsv", std::ios::app);
    for (const auto& e : fs::directory_iterator(runtime_dir, ec)) {
      if (e.path().extension() == L".dxbc") files.push_back(e.path());
      if (e.path().extension() != L".cpipe") continue;
      const std::string name = narrow(e.path().filename().wstring());
      if (indexed.count(name)) continue;
      std::string error;
      auto view = FileView::open(e.path(), error);
      if (!view) continue;
      const auto b = view->bytes();
      std::string markers;
      for (size_t at = 0; at + 16 <= b.size();) {
        uint64_t address;
        uint32_t size, kind;
        std::memcpy(&address, b.data() + at, 8);
        std::memcpy(&size, b.data() + at + 8, 4);
        std::memcpy(&kind, b.data() + at + 12, 4);
        at += 16;
        if (kind == 1 && at + 24 <= b.size()) {
          const std::string m = marker_of(b.subspan(at, 24));
          wanted.insert(m);
          markers += (markers.empty() ? "" : ",") + m;
        }
        at += size;
      }
      index << name << '\t' << markers << '\n';
      ++new_bundles;
    }
  }
  // The programs the game waited for (the runtime lists them in wanted-first.txt): compiled
  // before everything else.
  std::set<std::string> first;
  {
    std::ifstream in(out_dir / L"wanted-first.txt");
    for (std::string m; std::getline(in, m);) {
      if (!m.empty() && !fs::exists(out_dir / widen(m + ".dxil"), ec)) first.insert(m);
    }
  }
  for (const fs::path& f : containers(pack_dir)) files.push_back(f);
  std::printf("precompile: %zu programs wanted from the game's pipelines (%zu new bundles read), %zu the game waited for "
              "first, %zu containers to look through%s\n",
              wanted.size(), new_bundles, first.size(), files.size(), all ? " (all of their programs)" : "");
  std::fflush(stdout);
  std::atomic<size_t> next{0}, done{0}, skipped{0}, failed{0};
  std::mutex lock;
  std::ofstream failures(out_dir / L"failures.tsv", std::ios::app);
  // Round 0, when the game waited for some programs: those alone. Round 1: everything else.
  for (int round = first.empty() ? 1 : 0; round < 2; ++round) {
  next = 0;
  std::vector<std::thread> workers;
  for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w) {
    workers.emplace_back([&, round] {
      for (size_t i; (i = next.fetch_add(1)) < files.size();) {
        const bool runtime_file = files[i].parent_path() == runtime_dir;
        std::string error;
        const auto shader = load(files[i], error);
        if (!shader) continue;
        for (const shader::NativeBlob& b : shader->shader.blobs) {
          for (const shader::NativeProgram& p : b.programs) {
            if (p.stage != shader::HwStage::Vs && p.stage != shader::HwStage::Ps && p.stage != shader::HwStage::Cs) continue;
            const std::string marker = marker_of(b.code(p));
            if (round == 0 && !first.count(marker)) continue;
            if (!all && !runtime_file && !wanted.count(marker)) continue;
            const fs::path out = out_dir / widen(marker + ".dxil");
            const bool have = fs::exists(out, ec);
            if (have && hlsl_dir.empty()) {
              ++skipped;
              continue;
            }
            const shader::Translation t = shader::translate(input_of(shader->bytes, b, p), shader::Binding::Runtime);
            // With --raw, every wanted program's HLSL too (for reading it), compiled or not.
            if (!hlsl_dir.empty() && t.ok) {
              fs::create_directories(hlsl_dir, ec);
              std::ofstream(hlsl_dir / widen(marker + ".hlsl"), std::ios::binary) << t.hlsl;
            }
            if (have) {
              std::lock_guard<std::mutex> g(lock);
              ++skipped;
              continue;
            }
            std::string why = t.ok ? std::string() : t.failure;
            host::CompileResult c;
            if (t.ok) {
              c = host::compile_hlsl(t.hlsl, t.entry, t.profile);
              if (!c.ok) why = "DXC: " + c.messages.substr(0, 200);
            }
            std::lock_guard<std::mutex> g(lock);
            if (!why.empty()) {
              for (char& ch : why) {
                if (ch == '\n' || ch == '\t') ch = ' ';
              }
              failures << marker << '\t' << shader::hw_stage_name(p.stage) << '\t' << why << '\n';
              ++failed;
              continue;
            }
            std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(c.dxil.data()), static_cast<std::streamsize>(c.dxil.size()));
            ++done;
            std::printf("  %s %s\n", shader::hw_stage_name(p.stage), marker.c_str());
            std::fflush(stdout);
          }
        }
      }
    });
  }
  for (std::thread& t : workers) t.join();
  if (round == 0) {
    std::printf("precompile: the programs the game waited for are done (%zu compiled, %zu failed so far)\n", done.load(),
                failed.load());
    std::fflush(stdout);
  }
  }
  std::printf("precompile: %zu compiled, %zu there already, %zu failed (%s)\n", done.load(), skipped.load(), failed.load(),
              narrow((out_dir / L"failures.tsv").wstring()).c_str());
  return 0;
}

}  // namespace am::tools
