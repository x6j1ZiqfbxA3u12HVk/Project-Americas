// am-shaders translate / tsurvey: the shader translator on the corpus.

#include <windows.h>
#include <share.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <deque>
#include <map>
#include <set>
#include <span>
#include <mutex>
#include <thread>
#include <vector>

#include "core/file_view.h"
#include "core/json.h"
#include "core/sha256.h"
#include "core/text.h"
#include "host/dxc_compile.h"
#include "shader/console_program.h"
#include "shader/container.h"
#include "shader/rdna2.h"
#include "shader/service.h"
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

// A translation's content key: hex SHA-256 of profile, entry and source.
std::string source_key(const shader::Translation& t) {
  const std::string text = t.profile + "\n" + t.entry + "\n" + t.hlsl;
  const auto h = sha256(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  if (!h) return {};
  static const char* const kHex = "0123456789abcdef";
  std::string out;
  for (uint8_t v : *h) {
    out += kHex[v >> 4];
    out += kHex[v & 15];
  }
  return out;
}

int precompile(const fs::path& pack_dir, const fs::path& runtime_dir, const fs::path& out_dir, bool all, const fs::path& hlsl_dir,
               bool refresh) {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // Compiled DXIL by content key, shared by every translator version.
  const fs::path by_source = out_dir.parent_path() / L"by-source";
  fs::create_directories(by_source, ec);
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
  // Pass 1, over the containers in parallel: what each wanted program needs. Programs already
  // compiled, or whose source was compiled before (by-source), are settled here; the rest become
  // jobs, one a distinct source (the markers sharing it get the same DXIL).
  struct Job {
    size_t file = 0, blob = 0, program = 0;  // where the program is, to translate it again
    size_t hlsl_size = 0;
    bool first = false;
    std::string key;
    std::vector<std::pair<std::string, shader::HwStage>> markers;
  };
  std::vector<Job> jobs;
  std::map<std::string, size_t> job_of_key;
  std::set<std::string> claimed;
  std::atomic<size_t> next{0}, done{0}, skipped{0}, failed{0}, reused{0};
  std::mutex lock;
  std::ofstream failures(out_dir / L"failures.tsv", std::ios::app);
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  const auto fail = [&](const std::string& marker, shader::HwStage stage, std::string why) {
    for (char& ch : why) {
      if (ch == '\n' || ch == '\t') ch = ' ';
    }
    failures << marker << '\t' << shader::hw_stage_name(stage) << '\t' << why << '\n';
    ++failed;
  };
  {
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < threads; ++w) {
      workers.emplace_back([&] {
        for (size_t i; (i = next.fetch_add(1)) < files.size();) {
          const bool runtime_file = files[i].parent_path() == runtime_dir;
          std::string error;
          const auto shader = load(files[i], error);
          if (!shader) continue;
          for (size_t bi = 0; bi < shader->shader.blobs.size(); ++bi) {
            const shader::NativeBlob& b = shader->shader.blobs[bi];
            for (size_t pi = 0; pi < b.programs.size(); ++pi) {
              const shader::NativeProgram& p = b.programs[pi];
              if (p.stage != shader::HwStage::Vs && p.stage != shader::HwStage::Ps && p.stage != shader::HwStage::Cs) continue;
              const std::string marker = marker_of(b.code(p));
              if (!all && !runtime_file && !wanted.count(marker)) continue;
              {
                std::lock_guard<std::mutex> g(lock);
                if (!claimed.insert(marker).second) continue;
              }
              const fs::path out = out_dir / widen(marker + ".dxil");
              const fs::path sidecar = out_dir / widen(marker + ".src");
              const bool have = fs::exists(out, ec);
              if (have && hlsl_dir.empty() && !refresh) {
                ++skipped;
                continue;
              }
              const shader::Translation t = shader::translate(input_of(shader->bytes, b, p), shader::Binding::Runtime);
              // With --raw, every wanted program's HLSL too (for reading it), compiled or not.
              if (!hlsl_dir.empty() && t.ok) {
                fs::create_directories(hlsl_dir, ec);
                std::ofstream(hlsl_dir / widen(marker + ".hlsl"), std::ios::binary) << t.hlsl;
              }
              // The translation's content key: its source, entry and profile. DXIL is kept under
              // it too (by-source), so a source compiled once is never compiled again, under any
              // marker or translator version.
              const std::string key = t.ok ? source_key(t) : std::string();
              const fs::path stored = by_source / widen(key + ".dxil");
              if (have) {
                std::string was;
                std::ifstream in(sidecar);
                std::getline(in, was);
                in.close();
                // Unchanged, or files from before content keys (adopted as they are on a
                // refresh), or a program that no longer translates: kept.
                if (!t.ok || was == key || was.empty()) {
                  if (t.ok && was.empty()) {
                    std::ofstream(sidecar) << key;
                    if (!fs::exists(stored, ec)) fs::copy_file(out, stored, ec);
                  }
                  ++skipped;
                  continue;
                }
              }
              if (t.ok && fs::exists(stored, ec)) {  // the same source compiled before
                fs::copy_file(stored, out, fs::copy_options::overwrite_existing, ec);
                std::ofstream(sidecar) << key;
                ++reused;
                continue;
              }
              std::lock_guard<std::mutex> g(lock);
              if (!t.ok) {
                fail(marker, p.stage, t.failure);
                continue;
              }
              const auto [it, fresh] = job_of_key.emplace(key, jobs.size());
              if (fresh) jobs.push_back(Job{i, bi, pi, t.hlsl.size(), false, key, {}});
              Job& j = jobs[it->second];
              j.markers.emplace_back(marker, p.stage);
              j.first = j.first || first.count(marker);
            }
          }
        }
      });
    }
    for (std::thread& t : workers) t.join();
  }
  // Pass 2: the compiles, on every core: what the game waited for first, then the largest
  // sources first, so no long compile is left running alone at the end.
  std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) {
    if (a.first != b.first) return a.first;
    return a.hlsl_size > b.hlsl_size;
  });
  size_t hlsl_total = 0;
  for (const Job& j : jobs) hlsl_total += j.hlsl_size;
  std::printf("precompile: %zu sources to compile (%zu MiB of HLSL), %zu there already, %zu reused, %zu failed to translate; "
              "%u at a time\n",
              jobs.size(), hlsl_total >> 20, skipped.load(), reused.load(), failed.load(), threads);
  std::fflush(stdout);
  const auto start = std::chrono::steady_clock::now();
  std::atomic<size_t> hlsl_done{0};
  next = 0;
  {
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < threads; ++w) {
      workers.emplace_back([&] {
        for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
          const Job& j = jobs[i];
          std::string error;
          const auto shader = load(files[j.file], error);
          if (!shader) continue;
          const shader::NativeBlob& b = shader->shader.blobs[j.blob];
          const shader::Translation t =
              shader::translate(input_of(shader->bytes, b, b.programs[j.program]), shader::Binding::Runtime);
          const auto t0 = std::chrono::steady_clock::now();
          const host::CompileResult c = host::compile_hlsl(t.hlsl, t.entry, t.profile);
          const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
          std::lock_guard<std::mutex> g(lock);
          if (!c.ok) {
            for (const auto& [m, stage] : j.markers) fail(m, stage, "DXC: " + c.messages.substr(0, 200));
            continue;
          }
          const auto write = [&](const fs::path& f) {
            std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(c.dxil.data()), static_cast<std::streamsize>(c.dxil.size()));
          };
          write(by_source / widen(j.key + ".dxil"));
          for (const auto& [m, stage] : j.markers) {
            write(out_dir / widen(m + ".dxil"));
            std::ofstream(out_dir / widen(m + ".src")) << j.key;
          }
          ++done;
          hlsl_done += j.hlsl_size;
          const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
          const double left = hlsl_done ? elapsed * static_cast<double>(hlsl_total - hlsl_done) / static_cast<double>(hlsl_done) : 0;
          std::printf("  %zu/%zu %s %s, %zu KiB of HLSL, %.1f s (about %.0f min left)\n", done.load(), jobs.size(),
                      shader::hw_stage_name(j.markers[0].second), j.markers[0].first.c_str(), j.hlsl_size >> 10, took, left / 60);
          std::fflush(stdout);
        }
      });
    }
    for (std::thread& t : workers) t.join();
  }
  std::printf("precompile: %zu compiled, %zu reused from earlier compiles of the same source, %zu there already, %zu failed (%s)\n",
              done.load(), reused.load(), skipped.load(), failed.load(), narrow((out_dir / L"failures.tsv").wstring()).c_str());
  return 0;
}

// The shader service: compiles what the game waits for while it runs. Programs are found by
// marker in the game's shader containers (and the stream pipelines' containers the runtime
// saved), with no translation up front. Every second, wanted-first.txt (where the runtime lists
// what draws and dispatches wait for) is read for new markers, and those are compiled first;
// with none waiting, the rest of the game's pipelines' programs follow, smallest first. The
// runtime looks for a missing program again every two seconds, so a page fills in while it
// shows. One service at a time (a named mutex); runs until stopped, or until the stop event is
// set (a game run does that when it finds a newer am-shaders.exe, then starts that one).
namespace {

// The service's output: its console, and (--log) a file it appends to.
FILE* g_serve_log = nullptr;
void say(const char* format, ...) {
  va_list a;
  va_start(a, format);
  std::vfprintf(stdout, format, a);
  va_end(a);
  std::fflush(stdout);
  if (g_serve_log) {
    va_start(a, format);
    std::vfprintf(g_serve_log, format, a);
    va_end(a);
    std::fflush(g_serve_log);
  }
}

}  // namespace

int serve(const fs::path& pack_dir, const fs::path& runtime_dir, const fs::path& out_dir, unsigned jobs, const fs::path& log) {
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  if (!log.empty()) g_serve_log = _wfsopen(log.c_str(), L"a", _SH_DENYNO);
  SetConsoleTitleW(L"Project Americas - shader service");
  {
    SYSTEMTIME now;
    GetLocalTime(&now);
    say("serve: started %04u-%02u-%02u %02u:%02u:%02u, process %lu\n", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond, GetCurrentProcessId());
    std::fflush(stdout);
  }
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // Which build runs: the runtime compares it with the am-shaders.exe it would start.
  const auto record_self = [&] {
    const fs::path service_dir = out_dir.parent_path().parent_path() / shader::kServiceFolder;
    fs::create_directories(service_dir, ec);
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    std::ofstream(service_dir / shader::kServiceRunningFile) << narrow(self) << '\n';
  };
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, shader::kServiceStopEvent);
  HANDLE instance = CreateMutexW(nullptr, TRUE, shader::kServiceMutex);
  if (!instance) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // A replacement (the stop event is set: the running one finishes its compiles, then ends)
    // waits its turn; otherwise one is running already.
    if (!stop || WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
      say("serve: another am-shaders serve is running; this one ends\n");
      return 1;
    }
    record_self();  // the build that takes over: a run meanwhile starts no third one
    say("serve: the running service finishes its compiles first\n");
    std::fflush(stdout);
    const DWORD w = WaitForSingleObject(instance, INFINITE);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 1;
  }
  if (stop) ResetEvent(stop);
  record_self();
  const fs::path by_source = out_dir.parent_path() / L"by-source";
  fs::create_directories(by_source, ec);
  struct Location {
    size_t file = 0, blob = 0, program = 0, code_size = 0;
    shader::HwStage stage = shader::HwStage::Cs;
  };
  std::vector<fs::path> files;
  std::map<std::string, Location> where;
  std::mutex lock;
  // Maps the programs of `batch` (files appended to `files` from `first`).
  const auto map_files = [&](size_t first) {
    std::vector<fs::path> batch;
    {
      std::lock_guard<std::mutex> g(lock);
      batch.assign(files.begin() + static_cast<std::ptrdiff_t>(first), files.end());
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w) {
      workers.emplace_back([&] {
        for (size_t k; (k = next.fetch_add(1)) < batch.size();) {
          const size_t i = first + k;
          std::string error;
          const auto shader = load(batch[k], error);
          if (!shader) continue;
          for (size_t bi = 0; bi < shader->shader.blobs.size(); ++bi) {
            const shader::NativeBlob& b = shader->shader.blobs[bi];
            for (size_t pi = 0; pi < b.programs.size(); ++pi) {
              const shader::NativeProgram& pr = b.programs[pi];
              if (pr.stage != shader::HwStage::Vs && pr.stage != shader::HwStage::Ps && pr.stage != shader::HwStage::Cs) continue;
              const std::string marker = marker_of(b.code(pr));
              std::lock_guard<std::mutex> g(lock);
              where.try_emplace(marker, Location{i, bi, pi, b.code(pr).size(), pr.stage});
            }
          }
        }
      });
    }
    for (std::thread& t : workers) t.join();
  };
  std::set<fs::path> seen_runtime;
  const auto add_runtime_files = [&] {
    std::vector<fs::path> fresh;
    std::error_code dir_ec;
    for (const auto& e : fs::directory_iterator(runtime_dir, dir_ec)) {
      if (e.path().extension() == L".dxbc" && seen_runtime.insert(e.path()).second) fresh.push_back(e.path());
    }
    if (fresh.empty()) return size_t{0};
    size_t first = 0;
    {
      std::lock_guard<std::mutex> g(lock);
      first = files.size();
      files.insert(files.end(), fresh.begin(), fresh.end());
    }
    map_files(first);
    return fresh.size();
  };
  for (const fs::path& f : containers(pack_dir)) files.push_back(f);
  const auto t0 = std::chrono::steady_clock::now();
  map_files(0);
  add_runtime_files();
  // The rest: the game's pipelines' programs (cpipe-index.tsv, as the precompile wrote it).
  std::vector<std::string> bulk;
  {
    std::set<std::string> listed;
    std::ifstream in(out_dir / L"cpipe-index.tsv");
    for (std::string line; std::getline(in, line);) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      for (size_t at = tab + 1; at < line.size();) {
        const size_t comma = std::min(line.find(',', at), line.size());
        if (comma > at) listed.insert(line.substr(at, comma - at));
        at = comma + 1;
      }
    }
    for (const std::string& m : listed) {
      if (where.count(m) && !fs::exists(out_dir / widen(m + ".dxil"), ec)) bulk.push_back(m);
    }
    std::sort(bulk.begin(), bulk.end(), [&](const std::string& a, const std::string& b) { return where[a].code_size > where[b].code_size; });
  }
  say("serve: %zu programs mapped in %zu containers (%.0f s); %zu of the game's pipelines' programs not compiled yet; "
              "%u at a time\n",
              where.size(), files.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), bulk.size(), jobs);
  std::fflush(stdout);

  std::deque<std::string> urgent;  // what the game waits for, oldest first
  std::set<std::string> taken;     // compiled, compiling, failed or unknown this session
  std::ofstream failures(out_dir / L"failures.tsv", std::ios::app);
  std::atomic<size_t> compiled{0}, reused{0}, refused{0};
  std::map<std::string, size_t> refusals;  // by reason (its first 60 characters)
  int busy = 0;                            // workers compiling (under `lock`)
  bool stopping = false;                   // asked to stop: workers take nothing new
  int huge_busy = 0;                       // compiles of over 1 MiB of HLSL running (under `lock`)
  std::vector<std::pair<std::string, bool>> deferred_huge;  // large ones set aside (marker, waited)
  // Room for another large compile (under `lock`): none running, or 4.5 GB of RAM free.
  const auto huge_room = [&] {
    if (huge_busy == 0) return true;
    MEMORYSTATUSEX m{sizeof m};
    GlobalMemoryStatusEx(&m);
    return m.ullAvailPhys > (9ull << 29);
  };
  bool idle_told = false;
  // The watcher: new lines of wanted-first.txt.
  std::thread([&] {
    std::streamoff read = 0;
    for (;;) {
      std::ifstream in(out_dir / L"wanted-first.txt");
      if (in) {
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < read) read = 0;  // a new file
        in.seekg(read);
        std::vector<std::string> fresh;
        for (std::string m; std::getline(in, m);) {
          if (!m.empty() && m.back() == '\r') m.pop_back();
          if (!m.empty()) fresh.push_back(m);
        }
        read = size;
        bool unknown = false;
        for (const std::string& m : fresh) {
          if (fs::exists(out_dir / widen(m + ".dxil"), ec)) continue;
          std::lock_guard<std::mutex> g(lock);
          if (!where.count(m)) unknown = true;
          if (!taken.count(m)) urgent.push_back(m);
        }
        if (unknown) {
          if (const size_t added = add_runtime_files()) {
            std::lock_guard<std::mutex> g(lock);
            for (const std::string& m : fresh) {
              if (where.count(m) && !taken.count(m) && !fs::exists(out_dir / widen(m + ".dxil"), ec)) urgent.push_back(m);
            }
            say("serve: %zu new stream containers mapped\n", added);
          }
        }
      }
      Sleep(1000);
    }
  }).detach();

  const auto compile_one = [&](const std::string& marker, bool waited) {
    Location at;
    fs::path file;
    {
      std::lock_guard<std::mutex> g(lock);
      auto it = where.find(marker);
      if (it == where.end()) {
        say("serve: %s is in no container we have\n", marker.c_str());
        std::fflush(stdout);
        return;
      }
      at = it->second;
      file = files[at.file];
    }
    std::string error;
    const auto fail = [&](std::string why) {
      for (char& ch : why) {
        if (ch == '\n' || ch == '\t') ch = ' ';
      }
      std::lock_guard<std::mutex> g(lock);
      failures << marker << '\t' << shader::hw_stage_name(at.stage) << '\t' << why << '\n';
      failures.flush();
      ++refused;
      ++refusals[why.substr(0, 60)];
      say("  %srefused: %s %s: %s\n", waited ? "waited for, " : "", shader::hw_stage_name(at.stage), marker.c_str(),
                  why.substr(0, 120).c_str());
      std::fflush(stdout);
    };
    const auto shader = load(file, error);
    if (!shader) return fail("cannot read its container: " + error);
    const shader::NativeBlob& b = shader->shader.blobs[at.blob];
    const shader::Translation t = shader::translate(input_of(shader->bytes, b, b.programs[at.program]), shader::Binding::Runtime);
    if (!t.ok) return fail(t.failure);
    // The dispatcher form (one loop over a switch) of a large program keeps DXC busy for hours
    // (two of about 80,000 instructions ran past 45 minutes at 3 GB each): refused until such
    // programs get a better form, so they do not hold the large-compile slot.
    if (!t.structured && t.hlsl.size() > (1u << 20)) return fail("dispatcher form over 1 MiB of HLSL (DXC takes hours)");
    const std::string key = source_key(t);
    const fs::path stored = by_source / widen(key + ".dxil");
    if (fs::exists(stored, ec)) {
      fs::copy_file(stored, out_dir / widen(marker + ".dxil"), fs::copy_options::overwrite_existing, ec);
      std::ofstream(out_dir / widen(marker + ".src")) << key;
      ++reused;
      return;
    }
    const auto c0 = std::chrono::steady_clock::now();
    // The largest programs (over 1 MiB of HLSL take DXC minutes) unoptimized: twice as fast to
    // compile, and the driver optimizes the DXIL again when it makes the pipeline.
    const bool huge = t.hlsl.size() > (1u << 20);
    // DXC takes 3-4 GB for the largest: one more starts only while there is room for it (one
    // always may), so the service fits next to a game run and uses the cores when there is none.
    // One that finds no room is set aside and this worker takes the next program.
    if (huge) {
      std::lock_guard<std::mutex> g(lock);
      if (!huge_room()) {
        deferred_huge.emplace_back(marker, waited);
        return;
      }
      ++huge_busy;
    }
    const host::CompileResult c = host::compile_hlsl(t.hlsl, t.entry, t.profile, huge ? std::vector<std::wstring>{L"-O0"} : std::vector<std::wstring>{});
    if (huge) {
      std::lock_guard<std::mutex> g(lock);
      --huge_busy;
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    if (!c.ok) return fail("DXC: " + c.messages.substr(0, 200));
    const auto write = [&](const fs::path& f) {
      std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(c.dxil.data()), static_cast<std::streamsize>(c.dxil.size()));
    };
    // Under temporary names first: nothing reads half a file, even when the service is stopped
    // mid-write.
    write(by_source / widen(key + ".tmp"));
    fs::rename(by_source / widen(key + ".tmp"), by_source / widen(key + ".dxil"), ec);
    write(out_dir / widen(marker + ".tmp"));
    fs::rename(out_dir / widen(marker + ".tmp"), out_dir / widen(marker + ".dxil"), ec);
    std::ofstream(out_dir / widen(marker + ".src")) << key;
    ++compiled;
    std::lock_guard<std::mutex> g(lock);
    say("  %s%s %s, %zu KiB of HLSL, %.1f s (%zu compiled, %zu reused)\n", waited ? "waited for: " : "",
                shader::hw_stage_name(at.stage), marker.c_str(), t.hlsl.size() >> 10, took, compiled.load(), reused.load());
    std::fflush(stdout);
  };

  for (unsigned w = 0; w < jobs; ++w) {
    std::thread([&] {
      for (;;) {
        std::string marker;
        bool waited = false;
        {
          std::lock_guard<std::mutex> g(lock);
          if (stopping) return;  // asked to stop: no new work
          // A large program set aside, once a large-compile slot is free.
          if (!deferred_huge.empty() && huge_room()) {
            marker = deferred_huge.front().first;
            waited = deferred_huge.front().second;
            deferred_huge.erase(deferred_huge.begin());
          }
          while (!urgent.empty() && marker.empty()) {
            std::string m = urgent.front();
            urgent.pop_front();
            if (taken.insert(m).second) marker = m, waited = true;
          }
          while (marker.empty() && !bulk.empty()) {
            std::string m = bulk.back();
            bulk.pop_back();
            if (taken.insert(m).second) marker = m;
          }
          if (!marker.empty()) {
            ++busy;
            idle_told = false;
          } else if (busy == 0 && deferred_huge.empty() && !idle_told) {
            // Nothing left: say so, and why the rest is not compiled.
            idle_told = true;
            say("serve: idle, waiting for the game (%zu compiled, %zu reused, %zu refused by the translator)\n",
                        compiled.load(), reused.load(), refused.load());
            std::vector<std::pair<size_t, std::string>> top;
            for (const auto& [why, n] : refusals) top.emplace_back(n, why);
            std::sort(top.rbegin(), top.rend());
            for (size_t k = 0; k < top.size() && k < 20; ++k) say("  %5zu  %s\n", top[k].first, top[k].second.c_str());
            std::fflush(stdout);
          }
        }
        if (marker.empty()) {
          Sleep(200);
          continue;
        }
        if (!fs::exists(out_dir / widen(marker + ".dxil"), ec)) compile_one(marker, waited);
        std::lock_guard<std::mutex> g(lock);
        --busy;
      }
    }).detach();
  }
  // Until stopped (a newer build replaces this one): the compiles in flight finish (the largest
  // take many minutes, and the replacement would start them over), nothing new starts.
  if (stop) WaitForSingleObject(stop, INFINITE);
  else Sleep(INFINITE);
  {
    std::lock_guard<std::mutex> g(lock);
    stopping = true;
    say("serve: asked to stop; finishing %d compile(s) (%zu compiled, %zu reused)\n", busy, compiled.load(), reused.load());
    std::fflush(stdout);
  }
  for (;;) {
    {
      std::lock_guard<std::mutex> g(lock);
      if (busy == 0) break;
    }
    Sleep(500);
  }
  say("serve: stopped\n");
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}

}  // namespace am::tools
