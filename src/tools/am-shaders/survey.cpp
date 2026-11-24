// am-shaders survey / disasm: the console programs of the corpus through the RDNA 2 decoder.

#include "survey.h"

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
#include "shader/console_program.h"
#include "shader/container.h"
#include "shader/rdna2.h"

namespace fs = std::filesystem;
namespace rd = am::shader::rdna2;

namespace am::tools {
namespace {

constexpr uint32_t kCodeEnd = 0xBF9F0000;

struct Stats {
  uint64_t containers = 0, unreadable = 0, programs = 0, instructions = 0, dwords = 0, unreached_dwords = 0;
  uint64_t clean = 0;  // every instruction known, s_endpgm found, nothing but s_code_end after it
  std::map<std::string, uint64_t> by_name, by_encoding, by_stage, failures, unknown_opcodes, wave;
  std::map<std::string, std::string> failure_example;  // reason -> container
  void merge(const Stats& o) {
    containers += o.containers;
    unreadable += o.unreadable;
    programs += o.programs;
    instructions += o.instructions;
    dwords += o.dwords;
    unreached_dwords += o.unreached_dwords;
    clean += o.clean;
    for (const auto& [k, v] : o.by_name) by_name[k] += v;
    for (const auto& [k, v] : o.by_encoding) by_encoding[k] += v;
    for (const auto& [k, v] : o.by_stage) by_stage[k] += v;
    for (const auto& [k, v] : o.failures) failures[k] += v;
    for (const auto& [k, v] : o.unknown_opcodes) unknown_opcodes[k] += v;
    for (const auto& [k, v] : o.wave) wave[k] += v;
    for (const auto& [k, v] : o.failure_example) failure_example.emplace(k, v);
  }
  void fail(const std::string& reason, const std::string& where) {
    ++failures[reason];
    failure_example.emplace(reason, where);
  }
};

std::span<const uint32_t> as_dwords(std::span<const uint8_t> bytes) {
  return {reinterpret_cast<const uint32_t*>(bytes.data()), bytes.size() / 4};
}

// One program, by control flow from its entry: what it reaches must decode; what it never
// reaches (data after s_endpgm, the s_code_end padding) is counted apart.
void survey_program(std::span<const uint32_t> code, Stats& s, const std::string& where) {
  const rd::ProgramScan scan = rd::scan_program(code);
  bool clean = scan.errors == 0;
  if (scan.errors) {
    s.fail(scan.first_error_reason, where);
    // An unknown opcode stops a path: name it for the histogram.
    rd::Instruction insn;
    if (scan.first_error < code.size() && rd::decode(code, scan.first_error, insn) == rd::DecodeStatus::UnknownOpcode) {
      ++s.unknown_opcodes[std::string(rd::encoding_name(insn.encoding)) + " " + std::to_string(insn.opcode)];
    }
  }
  if (!scan.ends && !scan.indirect) {
    s.fail("no reachable s_endpgm", where);
    clean = false;
  }
  if (scan.indirect) ++s.wave["indirect jumps"];
  size_t reached = 0;
  for (uint32_t at : scan.starts) {
    rd::Instruction insn;
    rd::decode(code, at, insn);
    reached += insn.dwords;
    ++s.instructions;
    ++s.by_name[insn.info->name];
    ++s.by_encoding[rd::encoding_name(insn.encoding) + std::string(insn.form == rd::Form::Sdwa ? " sdwa"
                                                                   : insn.form == rd::Form::Dpp16 ? " dpp"
                                                                   : insn.form == rd::Form::Dpp8 ? " dpp8" : "")];
  }
  s.unreached_dwords += code.size() - reached;
  s.clean += clean;
}

void survey_file(const fs::path& file, Stats& s) {
  ++s.containers;
  const std::string where = narrow(file.filename().wstring());
  std::string error;
  auto view = FileView::open(file, error);
  if (!view) {
    ++s.unreadable;
    s.fail("unreadable file", where);
    return;
  }
  const auto c = shader::parse_container(view->bytes(), error);
  const shader::Part* dxil = c ? c->find("DXIL") : nullptr;
  if (!dxil) {
    s.fail("no DXIL part", where);
    return;
  }
  const auto console = shader::read_console_shader(dxil->data, error);
  if (!console) {
    s.fail("console blob: " + error, where);
    return;
  }
  for (const shader::NativeBlob& b : console->blobs) {
    for (const shader::NativeProgram& p : b.programs) {
      ++s.programs;
      ++s.by_stage[shader::hw_stage_name(p.stage)];
      ++s.wave[p.wave32 ? "wave32" : "wave64"];
      const auto code = as_dwords(b.code(p));
      s.dwords += code.size();
      survey_program(code, s, where);
    }
  }
}

std::vector<fs::path> corpus(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() == L".dxbc") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

void write_counts(JsonWriter& j, const char* key, const std::map<std::string, uint64_t>& m) {
  std::vector<std::pair<uint64_t, std::string>> v;
  for (const auto& [k, n] : m) v.emplace_back(n, k);
  std::sort(v.rbegin(), v.rend());
  j.key(key).begin_object();
  for (const auto& [n, k] : v) j.field(k, n);
  j.end_object();
}

}  // namespace

int survey(const fs::path& pack_dir, const fs::path& out_json) {
  const auto files = corpus(pack_dir);
  if (files.empty()) {
    std::fprintf(stderr, "no containers in %s: run am-shaders index first\n", narrow(pack_dir.wstring()).c_str());
    return 1;
  }
  const auto start = std::chrono::steady_clock::now();
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  std::atomic<size_t> next{0};
  std::mutex merge_lock;
  Stats total;
  const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < workers; ++t) {
    threads.emplace_back([&] {
      Stats mine;
      for (size_t i; (i = next.fetch_add(1)) < files.size();) survey_file(files[i], mine);
      std::lock_guard<std::mutex> lock(merge_lock);
      total.merge(mine);
    });
  }
  for (auto& t : threads) t.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  JsonWriter j;
  j.begin_object();
  j.field("containers", total.containers).field("programs", total.programs).field("clean_programs", total.clean)
      .field("instructions", total.instructions).field("code_dwords", total.dwords)
      .field("unreached_dwords", total.unreached_dwords).field("seconds", seconds);
  write_counts(j, "failures", total.failures);
  j.key("failure_examples").begin_object();
  for (const auto& [k, v] : total.failure_example) j.field(k, v);
  j.end_object();
  write_counts(j, "unknown_opcodes", total.unknown_opcodes);
  write_counts(j, "stages", total.by_stage);
  write_counts(j, "waves", total.wave);
  write_counts(j, "encodings", total.by_encoding);
  write_counts(j, "instructions_by_name", total.by_name);
  j.end_object();
  std::error_code ec;
  fs::create_directories(out_json.parent_path(), ec);
  std::ofstream(out_json, std::ios::binary) << j.str() << '\n';

  std::printf("%llu containers, %llu programs, %llu instructions (%s of code) in %.1f s\n",
              static_cast<unsigned long long>(total.containers), static_cast<unsigned long long>(total.programs),
              static_cast<unsigned long long>(total.instructions), format_bytes(total.dwords * 4).c_str(), seconds);
  std::printf("clean programs: %llu of %llu (%.2f %%)\n", static_cast<unsigned long long>(total.clean),
              static_cast<unsigned long long>(total.programs), total.programs ? 100.0 * total.clean / total.programs : 0.0);
  std::printf("distinct instructions: %zu\n", total.by_name.size());
  for (const auto& [k, n] : total.failures) std::printf("  failure: %-28s %llu\n", k.c_str(), static_cast<unsigned long long>(n));
  std::printf("wrote %s\n", narrow(out_json.wstring()).c_str());
  return 0;
}

int disasm(const std::string& wanted, const fs::path& pack_dir, const fs::path& raw_dir) {
  fs::path file;
  for (const fs::path& f : corpus(pack_dir)) {
    if (istarts_with_ascii(narrow(f.filename().wstring()), wanted)) {
      file = f;
      break;
    }
  }
  if (file.empty()) {
    std::fprintf(stderr, "no container starts with %s\n", wanted.c_str());
    return 1;
  }
  std::string error;
  auto view = FileView::open(file, error);
  const auto c = view ? shader::parse_container(view->bytes(), error) : std::nullopt;
  const shader::Part* dxil = c ? c->find("DXIL") : nullptr;
  const auto console = dxil ? shader::read_console_shader(dxil->data, error) : std::nullopt;
  if (!console) {
    std::fprintf(stderr, "%s: %s\n", narrow(file.wstring()).c_str(), error.c_str());
    return 1;
  }
  int index = 0;
  for (const shader::NativeBlob& b : console->blobs) {
    for (const shader::NativeProgram& p : b.programs) {
      const auto bytes = b.code(p);
      const auto code = as_dwords(bytes);
      std::printf("; %s program %d: %s, %s, %zu dwords, VGPRs %u, SGPRs %u, LDS %u bytes\n",
                  narrow(file.stem().wstring()).c_str(), index, shader::hw_stage_name(p.stage),
                  p.wave32 ? "wave32" : "wave64", code.size(), p.vgprs, p.sgprs, p.lds_bytes);
      if (!raw_dir.empty()) {
        std::error_code ec;
        fs::create_directories(raw_dir, ec);
        const fs::path out = raw_dir / widen(narrow(file.stem().wstring()) + "_" + std::to_string(index) + ".bin");
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      }
      // By control flow: what the program reaches, in address order; the rest is data.
      const rd::ProgramScan scan = rd::scan_program(code);
      size_t next = 0;
      for (uint32_t at : scan.starts) {
        if (at > next) std::printf("%06zx data, %zu dwords\n", next * 4, at - next);
        rd::Instruction insn;
        rd::decode(code, at, insn);
        std::printf("%06x %d %s\n", at * 4, insn.dwords, rd::describe(insn).c_str());
        next = at + insn.dwords;
      }
      if (next < code.size()) std::printf("%06zx data, %zu dwords (to the end)\n", next * 4, code.size() - next);
      if (scan.errors) {
        std::printf("; %zu errors, first at %06zx: %s\n", scan.errors, scan.first_error * 4, scan.first_error_reason.c_str());
      }
      ++index;
    }
  }
  return 0;
}

}  // namespace am::tools
