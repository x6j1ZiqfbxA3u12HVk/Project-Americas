// am-shaders translate / tsurvey: the shader translator on the corpus.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/file_view.h"
#include "core/json.h"
#include "core/text.h"
#include "host/dxc_compile.h"
#include "shader/console_program.h"
#include "shader/container.h"
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

std::optional<shader::ConsoleShader> load(const fs::path& file, std::string& error) {
  auto view = FileView::open(file, error);
  if (!view) return std::nullopt;
  const auto c = shader::parse_container(view->bytes(), error);
  const shader::Part* dxil = c ? c->find("DXIL") : nullptr;
  if (!dxil) {
    if (error.empty()) error = "no DXIL part";
    return std::nullopt;
  }
  return shader::read_console_shader(dxil->data, error);
}

shader::TranslateInput input_of(const shader::NativeBlob& b, const shader::NativeProgram& p) {
  shader::TranslateInput in;
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
  for (const fs::path& f : containers(pack_dir)) {
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
  for (const shader::NativeBlob& b : shader->blobs) {
    for (const shader::NativeProgram& p : b.programs) {
      const shader::Translation t = shader::translate(input_of(b, p), shader::Binding::Test);
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

int translation_survey(const fs::path& pack_dir, const fs::path& out_json, size_t compile_sample) {
  const auto files = containers(pack_dir);
  const auto start = std::chrono::steady_clock::now();
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  std::mutex lock;
  std::map<std::string, uint64_t> missing, stages_total, stages_ok;  // missing: programs blocked by it
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
        for (const shader::NativeBlob& b : shader->blobs) {
          for (const shader::NativeProgram& p : b.programs) {
            shader::Translation t = shader::translate(input_of(b, p), shader::Binding::Test);
            std::lock_guard<std::mutex> g(lock);
            ++stages_total[shader::hw_stage_name(p.stage)];
            if (t.ok) {
              ++stages_ok[shader::hw_stage_name(p.stage)];
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
  // DXC on a sample of what translated, in parallel.
  std::atomic<size_t> ci{0};
  std::atomic<uint64_t> compiled{0};
  std::map<std::string, std::string> compile_errors;
  workers.clear();
  for (unsigned w = 0; w < std::max(1u, std::thread::hardware_concurrency()); ++w) {
    workers.emplace_back([&] {
      for (size_t i; (i = ci.fetch_add(1)) < to_compile.size();) {
        const auto& [name, t] = to_compile[i];
        const host::CompileResult r = host::compile_hlsl(t.hlsl, t.entry, t.profile);
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
  for (const auto& [k, n] : stages_total) j.key(k).begin_object().field("total", n).field("translated", stages_ok[k]).end_object();
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
    std::printf("  %-3s %6llu programs, %6llu translated (%.1f %%)\n", k.c_str(), static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(stages_ok[k]), 100.0 * stages_ok[k] / n);
  }
  std::printf("DXC: %llu of %zu compiled\n", static_cast<unsigned long long>(compiled.load()), to_compile.size());
  std::printf("most blocking (programs held up by each):\n");
  for (size_t i = 0; i < m.size() && i < 25; ++i) std::printf("  %7llu  %s\n", static_cast<unsigned long long>(m[i].first), m[i].second.c_str());
  std::printf("wrote %s\n", narrow(out_json.wstring()).c_str());
  return 0;
}

}  // namespace am::tools
