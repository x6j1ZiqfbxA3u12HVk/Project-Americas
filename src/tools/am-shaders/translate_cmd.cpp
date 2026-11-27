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
#include "shader/dxil/dxil.h"
#include "shader/dxil/hlsl.h"
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
  if (p.stage == shader::HwStage::Vs || p.stage == shader::HwStage::Ps || p.stage == shader::HwStage::Ms) {
    std::string why;
    if (!shader::read_stage_io(container, b, p, in, why) && io_error) *io_error = why;
  }
  in.ms_threads = p.ms_threads;
  in.ms_max_vertices = p.ms_max_vertices;
  in.ms_max_primitives = p.ms_max_primitives;
  in.ms_user_sgprs = p.ms_user_sgprs;
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

std::string marker_of(std::span<const uint8_t> code);

int translate(const std::string& wanted, const fs::path& pack_dir, const fs::path& out_dir) {
  // A program's 48-digit marker: that program alone, as the shader service translates and
  // compiles it (runtime binding; -O0 over 1 MiB), with its form, size, nesting and compile time.
  if (wanted.size() == 48) {
    std::vector<fs::path> files = containers(pack_dir);
    std::error_code dir_ec;
    for (const auto& e : fs::directory_iterator(pack_dir.parent_path() / L"runtime", dir_ec)) {
      if (e.path().extension() == L".dxbc") files.push_back(e.path());
    }
    for (const fs::path& f : files) {
      std::string error;
      const auto shader = load(f, error);
      if (!shader) continue;
      for (const shader::NativeBlob& b : shader->shader.blobs) {
        for (const shader::NativeProgram& pr : b.programs) {
          if (marker_of(b.code(pr)) != wanted) continue;
          const shader::Translation t = shader::translate(input_of(shader->bytes, b, pr), shader::Binding::Runtime);
          if (!t.ok) {
            std::printf("%s: not translated: %s\n", wanted.c_str(), t.failure.c_str());
            return 1;
          }
          int depth = 0, deepest = 0;
          for (const char ch : t.hlsl) {
            if (ch == '{' || ch == '(') deepest = std::max(deepest, ++depth);
            else if (ch == '}' || ch == ')') --depth;
          }
          const bool chained = t.hlsl.find("uint am_go0 = 0u;") != std::string::npos;
          std::error_code ec;
          fs::create_directories(out_dir, ec);
          const fs::path hlsl = out_dir / widen(wanted + ".hlsl");
          std::ofstream(hlsl, std::ios::binary) << t.hlsl;
          std::printf("%s: %s, %s form, %s, %zu KiB of HLSL, nesting %d (nested form %d) (%s)\n", wanted.c_str(),
                      shader::hw_stage_name(pr.stage), !t.structured ? "dispatcher" : chained ? "chained structured" : "structured",
                      t.lanes ? "lanes as threads" : "the wave's masks", t.hlsl.size() >> 10, deepest, t.nested_depth,
                      narrow(hlsl.wstring()).c_str());
          std::fflush(stdout);
          // AM_SHADERS_O0=1: unoptimized over 1 MiB, as the service once did (to compare what the driver does).
          const bool huge = t.hlsl.size() > (1u << 20) && GetEnvironmentVariableA("AM_SHADERS_O0", nullptr, 0);
          const auto c0 = std::chrono::steady_clock::now();
          const host::CompileResult r =
              host::compile_hlsl(t.hlsl, t.entry, t.profile, huge ? std::vector<std::wstring>{L"-O0"} : std::vector<std::wstring>{});
          if (r.ok) {
            std::ofstream(out_dir / widen(wanted + ".dxil"), std::ios::binary)
                .write(reinterpret_cast<const char*>(r.dxil.data()), static_cast<std::streamsize>(r.dxil.size()));
          }
          std::printf("%s: %s in %.1f s (%zu bytes of DXIL)\n", wanted.c_str(), r.ok ? "compiled" : "compile failed",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count(), r.dxil.size());
          if (!r.ok) std::printf("%s\n", r.messages.substr(0, 2000).c_str());
          return r.ok ? 0 : 1;
        }
      }
    }
    std::fprintf(stderr, "%s: no program with that marker\n", wanted.c_str());
    return 1;
  }
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
  std::map<std::string, uint64_t> missing, stages_total, stages_ok, stages_structured, stages_lanes;  // missing: programs blocked by it
  std::map<std::string, uint64_t> mesh_shapes;  // mesh programs by their record's limits
  std::map<std::string, std::string> mesh_examples;  // a container of each
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
            if (p.stage == shader::HwStage::Ms) {
              char key[96];
              std::snprintf(key, sizeof key, "lanes %u, vertices %u, primitives %u, topology %u, user SGPRs %u", p.ms_threads,
                            p.ms_max_vertices, p.ms_max_primitives, p.ms_topology, p.ms_user_sgprs);
              if (!mesh_shapes.count(key)) mesh_examples[key] = narrow(files[i].stem().wstring());
              ++mesh_shapes[key];
            }
            if (t.ok) {
              ++stages_ok[shader::hw_stage_name(p.stage)];
              if (t.structured) ++stages_structured[shader::hw_stage_name(p.stage)];
              if (t.lanes) ++stages_lanes[shader::hw_stage_name(p.stage)];
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
    j.key(k).begin_object().field("total", n).field("translated", stages_ok[k]).field("structured", stages_structured[k]).field("lanes", stages_lanes[k]).end_object();
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
    std::printf("  %-3s %6llu programs, %6llu translated (%.1f %%), %llu structured, %llu lanes as threads\n", k.c_str(),
                static_cast<unsigned long long>(n), static_cast<unsigned long long>(stages_ok[k]), 100.0 * stages_ok[k] / n,
                static_cast<unsigned long long>(stages_structured[k]), static_cast<unsigned long long>(stages_lanes[k]));
  }
  std::printf("DXC: %llu of %zu compiled\n", static_cast<unsigned long long>(compiled.load()), to_compile.size());
  if (!mesh_shapes.empty()) {
    std::vector<std::pair<uint64_t, std::string>> shapes;
    for (const auto& [k, n] : mesh_shapes) shapes.emplace_back(n, k);
    std::sort(shapes.rbegin(), shapes.rend());
    std::printf("mesh programs by their limits:\n");
    for (size_t k = 0; k < shapes.size() && k < 15; ++k) {
      std::printf("  %6llu  %s (e.g. %s)\n", static_cast<unsigned long long>(shapes[k].first), shapes[k].second.c_str(),
                  mesh_examples[shapes[k].second].c_str());
    }
  }
  std::sort(slowest.rbegin(), slowest.rend());
  for (size_t i = 0; i < slowest.size() && i < 8; ++i) std::printf("  %7.1f s  %s\n", slowest[i].first, slowest[i].second.c_str());
  std::printf("most blocking (programs held up by each):\n");
  for (size_t i = 0; i < m.size() && i < 25; ++i) std::printf("  %7llu  %s\n", static_cast<unsigned long long>(m[i].first), m[i].second.c_str());
  std::printf("wrote %s\n", narrow(out_json.wstring()).c_str());
  return 0;
}

// The corpus through our own compiler: every program translated in the goto form (the runtime's
// binding), compiled by shader/dxil, and checked and signed by DXC's validator. What passes, by
// stage; why the rest does not, grouped; time spent. `every`: one container in that many.
int ours_survey(const fs::path& pack_dir, const fs::path& out_json, size_t every) {
  const auto all = containers(pack_dir);
  std::vector<fs::path> files;
  for (size_t i = 0; i < all.size(); i += std::max<size_t>(every, 1)) files.push_back(all[i]);
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  const auto start = std::chrono::steady_clock::now();
  struct Counts {
    uint64_t total = 0, translated = 0, compiled = 0, valid = 0, flags_differ = 0;
  };
  std::mutex lock, large;  // large: one program over 2 MiB of HLSL at a time (memory)
  std::map<std::string, Counts> by_stage;
  std::map<std::string, uint64_t> reasons;
  std::map<std::string, std::string> examples, flag_examples;
  std::map<std::string, uint64_t> flag_reasons;
  std::vector<std::pair<double, std::string>> slowest;
  double ours_seconds = 0, validate_seconds = 0;
  std::atomic<size_t> next{0}, done{0};
  // A message without its numbers (line numbers, value ids), so like refusals group together.
  const auto reason_of = [](std::string m) {
    if (const size_t at = m.find("\n"); at != std::string::npos) m.resize(at);
    std::string out;
    bool digits = false;
    for (const char ch : m) {
      if (ch >= '0' && ch <= '9') {
        if (!digits) out += '#';
        digits = true;
      } else {
        out += ch;
        digits = false;
      }
    }
    return out.substr(0, 110);
  };
  std::vector<std::thread> workers;
  const unsigned jobs = std::max(1u, std::thread::hardware_concurrency() - 2);
  for (unsigned w = 0; w < jobs; ++w) {
    workers.emplace_back([&] {
      for (size_t i; (i = next.fetch_add(1)) < files.size();) {
        std::string error;
        const auto shader = load(files[i], error);
        if (!shader) continue;
        for (const shader::NativeBlob& b : shader->shader.blobs) {
          for (const shader::NativeProgram& p : b.programs) {
            const std::string stage = shader::hw_stage_name(p.stage);
            shader::TranslateInput in = input_of(shader->bytes, b, p);
            in.goto_form = true;
            const shader::Translation t = shader::translate(in, shader::Binding::Runtime);
            std::string why;
            bool compiled = false, valid = false, flags_differ = false;
            std::string flag_note;
            double ours_s = 0, validate_s = 0;
            if (!t.ok) {
              why = "not translated: " + t.failure;
            } else {
              std::unique_lock<std::mutex> one(large, std::defer_lock);
              if (t.hlsl.size() > (2u << 20)) one.lock();
              const auto t0 = std::chrono::steady_clock::now();
              shader::dxil::Module m;
              std::string message;
              compiled = shader::dxil::compile_hlsl(t.hlsl, t.entry, m, message) && m.finish(message);
              const auto t1 = std::chrono::steady_clock::now();
              ours_s = std::chrono::duration<double>(t1 - t0).count();
              if (!compiled) {
                why = "ours: " + message;
              } else {
                host::CompileResult r = host::assemble_dxil(shader::dxil::write_bitcode(m));
                uint64_t flags = 0;
                if (!r.ok && shader::dxil::validator_flags(r.messages, flags)) {
                  flags_differ = true;
                  char d[80];
                  std::snprintf(d, sizeof d, "flags: the validator adds 0x%llx, removes 0x%llx",
                                static_cast<unsigned long long>(flags & ~m.shader_flags()),
                                static_cast<unsigned long long>(m.shader_flags() & ~flags));
                  flag_note = d;
                  m.set_flags(flags);
                  r = host::assemble_dxil(shader::dxil::write_bitcode(m));
                }
                valid = r.ok;
                if (!valid) {
                  // The validator's first error line.
                  const size_t e = r.messages.find("error:");
                  why = "validator: " + (e == std::string::npos ? r.messages : r.messages.substr(e + 7));
                }
              }
              validate_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            }
            std::lock_guard<std::mutex> g(lock);
            Counts& c = by_stage[stage];
            ++c.total;
            c.translated += t.ok;
            c.compiled += compiled;
            c.valid += valid;
            c.flags_differ += flags_differ;
            ours_seconds += ours_s;
            validate_seconds += validate_s;
            if (t.ok) slowest.emplace_back(ours_s, marker_of(b.code(p)) + " " + stage + " (" + std::to_string(t.hlsl.size() >> 10) + " KiB of HLSL)");
            if (!flag_note.empty() && !flag_reasons[flag_note]++) flag_examples[flag_note] = marker_of(b.code(p));
            if (!why.empty()) {
              const std::string k = reason_of(why);
              if (!reasons[k]++) examples[k] = marker_of(b.code(p));
            }
          }
        }
        if (const size_t d = ++done; d % 200 == 0) {
          std::fprintf(stderr, "  %zu of %zu containers\n", d, files.size());
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  Counts sum;
  for (const auto& [k, c] : by_stage) {
    sum.total += c.total;
    sum.translated += c.translated;
    sum.compiled += c.compiled;
    sum.valid += c.valid;
    sum.flags_differ += c.flags_differ;
  }
  std::vector<std::pair<uint64_t, std::string>> rs;
  for (const auto& [k, n] : reasons) rs.emplace_back(n, k);
  std::sort(rs.rbegin(), rs.rend());
  JsonWriter j;
  j.begin_object().field("translator_version", static_cast<uint64_t>(shader::kTranslatorVersion)).field("seconds", seconds);
  j.field("containers", static_cast<uint64_t>(files.size())).field("of_containers", static_cast<uint64_t>(all.size()));
  j.key("programs").begin_object();
  for (const auto& [k, c] : by_stage) {
    j.key(k).begin_object().field("total", c.total).field("translated", c.translated).field("compiled_by_ours", c.compiled);
    j.field("validated", c.valid).field("flags_differ", c.flags_differ).end_object();
  }
  j.end_object();
  j.key("refused").begin_object();
  for (const auto& [n, k] : rs) j.field(k + " (e.g. " + examples[k] + ")", n);
  j.end_object();
  j.end_object();
  std::ofstream(out_json, std::ios::binary) << j.str() << '\n';

  const auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
  std::printf("our compiler on %zu of %zu containers, %.0f s (ours %.0f s, validator %.0f s in all threads)\n", files.size(), all.size(),
              seconds, ours_seconds, validate_seconds);
  for (const auto& [k, c] : by_stage) {
    std::printf("  %-3s %6llu programs: translated %5.1f %%, compiled by ours %5.1f %%, validated %5.1f %% (flags differed: %llu)\n",
                k.c_str(), static_cast<unsigned long long>(c.total), pct(c.translated, c.total), pct(c.compiled, c.total),
                pct(c.valid, c.total), static_cast<unsigned long long>(c.flags_differ));
  }
  std::printf("  all %6llu programs: validated %.1f %% (%llu), of those translated %.1f %%\n", static_cast<unsigned long long>(sum.total),
              pct(sum.valid, sum.total), static_cast<unsigned long long>(sum.valid), pct(sum.valid, sum.translated));
  std::printf("refused, most common first:\n");
  for (size_t i = 0; i < rs.size() && i < 25; ++i) {
    std::printf("  %7llu  %s (e.g. %s)\n", static_cast<unsigned long long>(rs[i].first), rs[i].second.c_str(), examples[rs[i].second].c_str());
  }
  for (const auto& [k, n] : flag_reasons)
    std::printf("  %7llu  %s (e.g. %s)\n", static_cast<unsigned long long>(n), k.c_str(), flag_examples[k].c_str());
  std::sort(slowest.rbegin(), slowest.rend());
  std::printf("slowest in our compiler:\n");
  for (size_t i = 0; i < slowest.size() && i < 6; ++i) std::printf("  %7.2f s  %s\n", slowest[i].first, slowest[i].second.c_str());
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

// DXC's messages as a refusal's reason: its first error (the warnings before it say nothing of
// why), on one line, shortened.
std::string dxc_error(const std::string& messages) {
  size_t at = 0;
  for (size_t line = 0; line < messages.size();) {
    const size_t end = std::min(messages.find('\n', line), messages.size());
    const std::string_view l(messages.data() + line, end - line);
    if (l.find("error:") != std::string_view::npos && l.find("validation errors") == std::string_view::npos) {
      at = line;
      break;
    }
    line = end + 1;
  }
  std::string r = messages.substr(at, 200);
  for (char& ch : r) {
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  }
  // Drop the file and line ("hlsl.hlsl:720: "): the program's marker says which.
  const size_t e = r.find("error:");
  return e == std::string::npos ? r : r.substr(e);
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
            for (const auto& [m, stage] : j.markers) fail(m, stage, "DXC: " + dxc_error(c.messages));
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

// Whether a program can loop: a backward branch (or far jump), or a control flow the translator
// runs as a dispatcher. Those are the programs the runaway guards matter for.
bool has_loop(std::span<const uint32_t> code) {
  namespace rd = shader::rdna2;
  using rd::Field;
  const rd::ProgramScan scan = rd::scan_program(code);
  for (const auto& [at, target] : scan.far_jumps) {
    if (target <= at) return true;
  }
  for (const uint32_t at : scan.starts) {
    rd::Instruction insn;
    if (rd::decode(code, at, insn) != rd::DecodeStatus::Ok || !(insn.info->flags & rd::kBranch)) continue;
    if (!insn.has_field(Field::SIMM16)) continue;
    if (static_cast<int16_t>(insn.field(Field::SIMM16)) < 0) return true;
  }
  return false;
}

// am-shaders guard-loops [apply]: the compiled programs (dxil\rt<N>\<marker>.dxil) that can loop,
// counted; with `apply` their DXIL is removed so the shader service compiles them again with
// the runaway guards (translations from before the guards could hang the GPU).
int invalidate_loops(const fs::path& pack_dir, const fs::path& runtime_dir, const fs::path& out_dir, bool apply) {
  std::vector<fs::path> files = containers(pack_dir);
  for (const fs::path& f : containers(runtime_dir)) files.push_back(f);
  std::set<std::string> seen;
  size_t programs = 0, looping = 0, removed = 0;
  for (const fs::path& f : files) {
    std::string error;
    const auto shader = load(f, error);
    if (!shader) continue;
    for (const shader::NativeBlob& b : shader->shader.blobs) {
      for (const shader::NativeProgram& pr : b.programs) {
        if (pr.stage != shader::HwStage::Vs && pr.stage != shader::HwStage::Ps && pr.stage != shader::HwStage::Cs) continue;
        const auto bytes = b.code(pr);
        const std::string marker = marker_of(bytes);
        if (!seen.insert(marker).second) continue;
        std::error_code ec;
        const fs::path dxil = out_dir / widen(marker + ".dxil");
        if (!fs::exists(dxil, ec)) continue;
        ++programs;
        std::vector<uint32_t> code(bytes.size() / 4);
        std::memcpy(code.data(), bytes.data(), code.size() * 4);
        if (!has_loop(code)) continue;
        ++looping;
        if (apply) {
          removed += fs::remove(dxil, ec);
          fs::remove(out_dir / widen(marker + ".src"), ec);
        }
      }
    }
  }
  std::printf("guard-loops: %zu compiled programs, %zu of them can loop%s\n", programs, looping,
              apply ? (", " + std::to_string(removed) + " removed for compiling again").c_str() : " (dry run; 'apply' removes them)");
  return 0;
}

int serve(const fs::path& pack_dir, const fs::path& runtime_dir, const fs::path& out_dir, unsigned jobs, const fs::path& log,
          bool everything) {
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
  // Whether this service holds the mutex (and so answers the stop event) yet.
  std::atomic<bool> owner{true};
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // A replacement (the stop event is set: the running one finishes its compiles, then ends)
    // starts work at once, skipping what the old one is compiling (compiling\<marker>), and
    // takes the mutex when the old one ends; otherwise one is running already.
    if (!stop || WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
      say("serve: another am-shaders serve is running; this one ends\n");
      return 1;
    }
    owner = false;
    say("serve: the service it replaces finishes its compiles meanwhile\n");
    std::thread([&owner, instance, stop] {
      const DWORD w = WaitForSingleObject(instance, INFINITE);
      if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
        ResetEvent(stop);
        owner = true;
      }
    }).detach();
  } else if (stop) {
    ResetEvent(stop);
  }
  record_self();  // the build that runs: a run meanwhile starts no third one
  // Programs being compiled, by any service: compiling\<marker> holds the process id.
  const fs::path compiling_dir = out_dir / L"compiling";
  fs::create_directories(compiling_dir, ec);
  const auto compiled_elsewhere = [&](const std::string& marker) {
    std::ifstream in(compiling_dir / widen(marker));
    DWORD pid = 0;
    if (!(in >> pid) || pid == GetCurrentProcessId()) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
  };
  const fs::path by_source = out_dir.parent_path() / L"by-source";
  fs::create_directories(by_source, ec);
  struct Location {
    size_t file = 0, blob = 0, program = 0, code_size = 0;
    shader::HwStage stage = shader::HwStage::Cs;
    bool loops = false;  // it can loop: compiled before the runaway guards, it is compiled again
  };
  // Programs compiled with the runaway guards (guarded.txt, one marker a line). A program that
  // can loop and is not listed is compiled again: first when the game waits for it, the rest
  // when nothing else is left. Its old DXIL serves until then.
  std::set<std::string> guarded;
  {
    std::ifstream in(out_dir / L"guarded.txt");
    for (std::string m; std::getline(in, m);) {
      if (!m.empty() && m.back() == '\r') m.pop_back();
      if (!m.empty()) guarded.insert(m);
    }
  }
  std::ofstream guarded_out(out_dir / L"guarded.txt", std::ios::app);
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
              if (pr.stage != shader::HwStage::Vs && pr.stage != shader::HwStage::Ps && pr.stage != shader::HwStage::Cs &&
                  pr.stage != shader::HwStage::Ms) {
                continue;
              }
              const std::string marker = marker_of(b.code(pr));
              std::vector<uint32_t> code(b.code(pr).size() / 4);
              std::memcpy(code.data(), b.code(pr).data(), code.size() * 4);
              const bool loops = has_loop(code);
              std::lock_guard<std::mutex> g(lock);
              where.try_emplace(marker, Location{i, bi, pi, b.code(pr).size(), pr.stage, loops});
            }
          }
        }
      });
    }
    for (std::thread& t : workers) t.join();
  };
  // DXIL compiled unoptimized (-O0, once used for programs over 1 MiB of HLSL to halve DXC's
  // time): AMD's driver compiler takes over 20 GB on such DXIL when the game makes a pipeline
  // from it (the optimized build of the same program: 6.8 GB at its peak, 87 MB kept). Every
  // compile is optimized now; these are compiled again, the game's first. An optimized large
  // program's .src key ends in "-o3"; large DXIL whose key does not was compiled at -O0.
  std::set<std::string> unoptimized;
  {
    std::error_code e;
    for (const auto& f : fs::directory_iterator(out_dir, e)) {
      if (f.path().extension() != L".dxil" || f.file_size(e) < (1u << 20)) continue;
      std::string key;
      std::ifstream(fs::path(f.path()).replace_extension(L".src")) >> key;
      if (key.size() < 3 || key.compare(key.size() - 3, 3, "-o3") != 0) unoptimized.insert(narrow(f.path().stem().wstring()));
    }
  }
  // Programs compiled from an older translation: the translator changed what they become (its
  // version did not), found in the background (below) and compiled again when idle.
  std::vector<std::string> stale;
  std::set<std::string> stale_set;
  // Under `lock`: whether `m` needs compiling: no DXIL yet, DXIL compiled unoptimized or from an
  // older translation, or DXIL from before the guards for a program that can loop.
  const auto needs = [&](const std::string& m) {
    std::error_code e;
    if (!fs::exists(out_dir / widen(m + ".dxil"), e)) return true;
    if (unoptimized.count(m) || stale_set.count(m)) return true;
    const auto it = where.find(m);
    return it != where.end() && it->second.loops && !guarded.count(m);
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
  std::vector<std::string> upgrades;  // compiled before the guards, able to loop: last
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
    // What the game asks for (wanted-first.txt) comes first, always. When nothing waits (and
    // not `demand`), the game's other pipelines' programs, then programs compiled before the
    // runaway guards, on all workers but two.
    for (const std::string& m : listed) {
      if (everything && where.count(m) && !fs::exists(out_dir / widen(m + ".dxil"), ec)) bulk.push_back(m);
    }
    // Compiled before the guards and able to loop: again, after everything else.
    for (const auto& [m, at] : where) {
      if (everything && at.loops && !guarded.count(m) && fs::exists(out_dir / widen(m + ".dxil"), ec)) upgrades.push_back(m);
    }
    std::sort(upgrades.begin(), upgrades.end(), [&](const std::string& a, const std::string& b) { return where[a].code_size > where[b].code_size; });
    // Unoptimized DXIL is compiled again on demand only: the runtime asks for a large program whose
    // DXIL is not optimized as if it were missing (host_pipeline.cpp), and needs() takes it then.
    // Done as idle work, the largest ones held the one large-compile slot for many minutes while
    // a program the game waited for queued behind them.
    std::sort(bulk.begin(), bulk.end(), [&](const std::string& a, const std::string& b) { return where[a].code_size > where[b].code_size; });
  }
  say("serve: %zu programs mapped in %zu containers (%.0f s); %s; %u at a time\n", where.size(), files.size(),
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
      everything ? ("everything: " + std::to_string(bulk.size()) + " of the game's pipelines' programs not compiled yet, " +
                    std::to_string(upgrades.size()) + " to compile again with the runaway guards").c_str()
                 : "on demand: what the game asks for", jobs);
  std::fflush(stdout);

  // Which compiled programs the translator now makes differently: each one translated again and
  // its source key compared with the one its DXIL was made from (.src). A translator change
  // thus rebuilds only the programs it changes, without a new version and a rebuild of all.
  if (everything) {
    std::thread([&] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
      std::vector<std::pair<std::string, Location>> made;
      {
        std::lock_guard<std::mutex> g(lock);
        std::error_code e;
        for (const auto& [m, at] : where) {
          if (fs::exists(out_dir / widen(m + ".dxil"), e)) made.emplace_back(m, at);
        }
      }
      size_t found = 0;
      for (const auto& [m, at] : made) {
        std::string stored;
        std::ifstream(out_dir / widen(m + ".src")) >> stored;
        if (stored.empty()) continue;  // compiled before keys were kept: left as it is
        fs::path file;
        {
          std::lock_guard<std::mutex> g(lock);
          file = files[at.file];
        }
        std::string error;
        const auto shader = load(file, error);
        if (!shader) continue;
        const shader::NativeBlob& b = shader->shader.blobs[at.blob];
        const shader::Translation t = shader::translate(input_of(shader->bytes, b, b.programs[at.program]), shader::Binding::Runtime);
        if (!t.ok) continue;
        const std::string key = source_key(t) + (t.hlsl.size() > (1u << 20) ? "-o3" : "");
        if (key == stored) continue;
        std::lock_guard<std::mutex> g(lock);
        if (stale_set.insert(m).second) stale.push_back(m);
        ++found;
      }
      say("serve: %zu of %zu compiled programs come from an older translation: compiled again when idle\n", found, made.size());
      std::fflush(stdout);
    }).detach();
  }

  std::deque<std::string> urgent;  // what the game waits for, oldest first
  std::set<std::string> taken;     // compiled, compiling, failed or unknown this session
  // This build (its executable's size and time): a program it refused before is not tried
  // again (failures.tsv's fourth column); a new build tries them all once more.
  const std::string build = [] {
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(self.c_str(), GetFileExInfoStandard, &a)) return std::string("unknown");
    char text[48];
    std::snprintf(text, sizeof text, "%llx-%llx", (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
                  (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime);
    return std::string(text);
  }();
  {
    size_t known = 0;
    std::ifstream in(out_dir / L"failures.tsv");
    for (std::string line; std::getline(in, line);) {
      const size_t last = line.rfind('\t');
      if (last == std::string::npos || line.compare(last + 1, std::string::npos, build) != 0) continue;
      const std::string marker = line.substr(0, line.find('\t'));
      if (taken.insert(marker).second) ++known;
    }
    if (known) say("serve: %zu programs this build refused before are not tried again (failures.tsv)\n", known);
  }
  std::ofstream failures(out_dir / L"failures.tsv", std::ios::app);
  std::atomic<size_t> compiled{0}, reused{0}, refused{0};
  std::map<std::string, size_t> refusals;  // by reason (its first 60 characters)
  int busy = 0;                            // workers compiling (under `lock`)
  bool stopping = false;                   // asked to stop: workers take nothing new
  int idle_busy = 0;                       // workers on idle work (not asked for by the game)
  int huge_busy = 0;                       // compiles of over 1 MiB of HLSL running (under `lock`)
  std::vector<std::pair<std::string, bool>> deferred_huge;  // large ones set aside (marker, waited)
  // Compiles in progress (DXC gives no progress of its own): large ones are announced when they
  // start, and every 5 minutes the ones running over 3 minutes are listed with their time so far.
  struct Running {
    std::chrono::steady_clock::time_point since;
    size_t hlsl_kib;
    const char* stage;
  };
  std::map<std::string, Running> running;
  std::thread([&] {
    for (;;) {
      Sleep(5 * 60 * 1000);
      std::lock_guard<std::mutex> g(lock);
      const auto now = std::chrono::steady_clock::now();
      for (const auto& [m, r] : running) {
        const double minutes = std::chrono::duration<double>(now - r.since).count() / 60;
        if (minutes < 3) continue;
        say("  still compiling %s %s (%zu KiB of HLSL): %.0f min so far\n", r.stage, m.c_str(), r.hlsl_kib, minutes);
      }
      for (const auto& [m, waited] : deferred_huge) {
        if (waited) say("  waiting for the large-compile slot: %s (the game waits for it)\n", m.c_str());
      }
      std::fflush(stdout);
    }
  }).detach();
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
          std::lock_guard<std::mutex> g(lock);
          if (!needs(m)) continue;
          if (!where.count(m)) unknown = true;
          if (!taken.count(m)) urgent.push_back(m);
        }
        if (unknown) {
          if (const size_t added = add_runtime_files()) {
            std::lock_guard<std::mutex> g(lock);
            for (const std::string& m : fresh) {
              if (where.count(m) && !taken.count(m) && needs(m)) urgent.push_back(m);
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
      failures << marker << '\t' << shader::hw_stage_name(at.stage) << '\t' << why << '\t' << build << '\n';
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
    // Large programs' keys say they were compiled optimized: a stored -O0 build is never reused.
    const std::string key = source_key(t) + (t.hlsl.size() > (1u << 20) ? "-o3" : "");
    const fs::path stored = by_source / widen(key + ".dxil");
    if (fs::exists(stored, ec)) {
      fs::copy_file(stored, out_dir / widen(marker + ".dxil"), fs::copy_options::overwrite_existing, ec);
      std::ofstream(out_dir / widen(marker + ".src")) << key;
      ++reused;
      std::lock_guard<std::mutex> g(lock);
      guarded.insert(marker);
      guarded_out << marker << '\n' << std::flush;
      return;
    }
    const auto c0 = std::chrono::steady_clock::now();
    // Programs over 1 MiB of HLSL take DXC minutes and 3-4 GB; they are compiled optimized all the
    // same (see `unoptimized`: the driver cannot cope with unoptimized ones).
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
    // A compile the compiler could not run (not one it refused: out of memory while other
    // compiles or a game run hold it, for one) is tried again a little later, and never counted
    // as a refusal: a refusal is not tried again by this build.
    {
      std::lock_guard<std::mutex> g(lock);
      running[marker] = {std::chrono::steady_clock::now(), t.hlsl.size() >> 10, shader::hw_stage_name(at.stage)};
      if (huge) {
        say("  compiling large %s %s (%zu KiB of HLSL)%s: DXC gives no progress; a line every 5 minutes\n",
            shader::hw_stage_name(at.stage), marker.c_str(), t.hlsl.size() >> 10, waited ? ", the game waits for it" : "");
        std::fflush(stdout);
      }
    }
    host::CompileResult c;
    for (int attempt = 0;; ++attempt) {
      c = host::compile_hlsl(t.hlsl, t.entry, t.profile);
      if (c.ok || !c.transient || attempt == 3) break;
      static constexpr DWORD kWaitMs[] = {5000, 20000, 60000};
      say("  %s %s: DXC %s; trying again in %lu s\n", shader::hw_stage_name(at.stage), marker.c_str(), c.messages.c_str(),
          static_cast<unsigned long>(kWaitMs[attempt] / 1000));
      std::fflush(stdout);
      Sleep(kWaitMs[attempt]);
    }
    {
      std::lock_guard<std::mutex> g(lock);
      running.erase(marker);
      if (huge) --huge_busy;
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    if (!c.ok && c.transient) {
      say("  %s %s: DXC %s four times; left for the next service run (not recorded as refused)\n",
          shader::hw_stage_name(at.stage), marker.c_str(), c.messages.c_str());
      std::fflush(stdout);
      return;
    }
    if (!c.ok) return fail("DXC: " + dxc_error(c.messages));
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
    unoptimized.erase(marker);
    guarded.insert(marker);
    guarded_out << marker << '\n' << std::flush;
    // Progress: what is left and when it should be done, from the pace of the last 40 compiles
    // in program code (the queues run smallest first, so a plain count would promise too much).
    static std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> recent;
    const auto now = std::chrono::steady_clock::now();
    recent.emplace_back(now, at.code_size);
    if (recent.size() > 40) recent.pop_front();
    uint64_t left_code = 0, recent_code = 0;
    const size_t left = urgent.size() + bulk.size() + stale.size() + upgrades.size() + deferred_huge.size();
    for (const std::string& m : urgent) left_code += where[m].code_size;
    for (const std::string& m : bulk) left_code += where[m].code_size;
    for (const std::string& m : upgrades) left_code += where[m].code_size;
    for (const std::string& m : stale) left_code += where[m].code_size;
    for (const auto& d : deferred_huge) left_code += where[d.first].code_size;
    for (const auto& r : recent) recent_code += r.second;
    const double window = std::chrono::duration<double>(now - recent.front().first).count();
    std::string eta = "estimating";
    if (recent.size() >= 10 && window > 0 && recent_code) {
      const uint64_t seconds = static_cast<uint64_t>(static_cast<double>(left_code) / (recent_code / window));
      char text[48];
      std::snprintf(text, sizeof text, "about %lluh %02llum to go", static_cast<unsigned long long>(seconds / 3600),
                    static_cast<unsigned long long>(seconds / 60 % 60));
      eta = text;
    }
    say("  %s%s %s, %zu KiB of HLSL, %.1f s (%zu compiled, %zu reused) [%zu left; %s]\n", waited ? "waited for: " : "",
                shader::hw_stage_name(at.stage), marker.c_str(), t.hlsl.size() >> 10, took, compiled.load(), reused.load(), left,
                eta.c_str());
    std::fflush(stdout);
  };

  for (unsigned w = 0; w < jobs; ++w) {
    std::thread([&] {
      for (;;) {
        std::string marker;
        bool waited = false, idle_item = false;
        {
          std::lock_guard<std::mutex> g(lock);
          if (stopping) return;  // asked to stop: no new work
          idle_item = false;
          // A large program set aside, once a large-compile slot is free.
          // One the game waits for first.
          if (!deferred_huge.empty() && huge_room()) {
            auto pick = std::find_if(deferred_huge.begin(), deferred_huge.end(), [](const auto& d) { return d.second; });
            if (pick == deferred_huge.end()) pick = deferred_huge.begin();
            marker = pick->first;
            waited = pick->second;
            deferred_huge.erase(pick);
          }
          while (!urgent.empty() && marker.empty()) {
            std::string m = urgent.front();
            urgent.pop_front();
            if (taken.insert(m).second) marker = m, waited = true;
          }
          // Idle work (nothing the game waits for): at most all workers but two, so two are free
          // for the game's next request (a compile in progress cannot be stopped).
          const bool idle_room = idle_busy + 2 < static_cast<int>(jobs) || (jobs <= 2 && idle_busy == 0);
          while (marker.empty() && idle_room && !bulk.empty()) {
            std::string m = bulk.back();
            bulk.pop_back();
            if (taken.insert(m).second) marker = m, idle_item = true;
          }
          while (marker.empty() && idle_room && !stale.empty()) {
            std::string m = stale.back();
            stale.pop_back();
            if (taken.insert(m).second) marker = m, idle_item = true;
          }
          while (marker.empty() && idle_room && !upgrades.empty()) {
            std::string m = upgrades.back();
            upgrades.pop_back();
            if (taken.insert(m).second) marker = m, idle_item = true;
          }
          if (idle_item) ++idle_busy;
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
        bool wanted_compile = false;
        {
          std::lock_guard<std::mutex> g(lock);
          wanted_compile = needs(marker);
        }
        if (wanted_compile && !compiled_elsewhere(marker)) {
          std::ofstream(compiling_dir / widen(marker)) << GetCurrentProcessId();
          compile_one(marker, waited);
          std::error_code rm;
          fs::remove(compiling_dir / widen(marker), rm);
        }
        std::lock_guard<std::mutex> g(lock);
        --busy;
        if (idle_item) --idle_busy;
      }
    }).detach();
  }
  // Until stopped (a newer build replaces this one): the compiles in flight finish (the largest
  // take many minutes, and the replacement would start them over), nothing new starts. A
  // replacement answers the stop event only once it holds the mutex (the event is still set
  // for the service it replaced until then).
  while (!owner) Sleep(500);
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
