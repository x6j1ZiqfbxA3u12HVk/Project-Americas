// am-cpuscan: which CPU instruction sets the title's code uses, and where it uses the ones this
// host lacks (those the loader must patch or emulate). Read-only.
//
//   am-cpuscan [<exe|dll|folder>] [--sites] [--json <file>]
//   am-cpuscan <exe|dll> --boundaries <va-from>,<va-to>
//
// Without a path it scans game_dir from americas.ini. --boundaries prints the instructions of a
// virtual address range as decoded linearly (address, length, required feature): a check against
// another disassembler.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/file_view.h"
#include "core/json.h"
#include "core/text.h"
#include "host/cpu.h"
#include "pe/pe_image.h"
#include "x86/code_scan.h"
#include "x86/decoder.h"

namespace fs = std::filesystem;
using namespace am;
using host::CpuFeature;

namespace {

struct Options {
  fs::path target;
  bool sites = false;
  fs::path json;
  std::optional<std::pair<uint64_t, uint64_t>> boundaries;
};

int usage() {
  std::fprintf(stderr,
               "usage: am-cpuscan [<exe|dll|folder>] [--sites] [--json <file>]\n"
               "       am-cpuscan <exe|dll> --boundaries <va-from>,<va-to>\n");
  return 2;
}

std::optional<uint64_t> parse_number(std::wstring s) {
  int base = 10;
  if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) {
    s = s.substr(2);
    base = 16;
  }
  if (s.empty()) return std::nullopt;
  wchar_t* end = nullptr;
  const unsigned long long v = wcstoull(s.c_str(), &end, base);
  if (!end || *end) return std::nullopt;
  return v;
}

std::optional<Options> parse_args(int argc, wchar_t** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--sites") {
      o.sites = true;
    } else if (a == L"--json" && i + 1 < argc) {
      o.json = argv[++i];
    } else if (a == L"--boundaries" && i + 1 < argc) {
      const std::wstring range = argv[++i];
      const size_t comma = range.find(L',');
      if (comma == std::wstring::npos) return std::nullopt;
      const auto from = parse_number(range.substr(0, comma));
      const auto to = parse_number(range.substr(comma + 1));
      if (!from || !to || *to <= *from) return std::nullopt;
      o.boundaries = std::make_pair(*from, *to);
    } else if (!a.empty() && a[0] != L'-' && o.target.empty()) {
      o.target = a;
    } else {
      return std::nullopt;
    }
  }
  return o;
}

bool has_extension(const fs::path& p, std::wstring_view ext) {
  const std::wstring e = p.extension().wstring();
  return CompareStringOrdinal(e.c_str(), static_cast<int>(e.size()), ext.data(), static_cast<int>(ext.size()), TRUE) ==
         CSTR_EQUAL;
}

// The features that matter for running console code on another CPU: those a PC may lack.
std::vector<CpuFeature> features_to_locate(const host::CpuidSnapshot& cpu) {
  std::vector<CpuFeature> v;
  for (const host::CpuFeatureInfo& info : host::console_cpu_features()) {
    if (!host::has_feature(cpu, info.feature)) v.push_back(info.feature);
  }
  return v;
}

int print_boundaries(const pe::Image& img, std::span<const uint8_t> file, uint64_t from, uint64_t to) {
  if (from < img.image_base || to - img.image_base > 0xFFFFFFFFull) {
    std::fprintf(stderr, "range outside the image\n");
    return 1;
  }
  const auto from_rva = static_cast<uint32_t>(from - img.image_base);
  const auto size = static_cast<uint32_t>(to - from);
  // Allow decoding a little past the end so the last instruction is complete.
  auto bytes = pe::bytes_at(img, file, from_rva, size + 16);
  if (!bytes) bytes = pe::bytes_at(img, file, from_rva, size);
  if (!bytes) {
    std::fprintf(stderr, "range not in one section's file data\n");
    return 1;
  }
  size_t i = 0;
  while (i < size) {
    x86::Instruction insn;
    const x86::DecodeStatus st = x86::decode(bytes->subspan(i), insn);
    const uint64_t va = from + i;
    if (st != x86::DecodeStatus::Ok) {
      std::printf("%016llX: ?? %02x\n", static_cast<unsigned long long>(va), (*bytes)[i]);
      ++i;
      continue;
    }
    const x86::Requirement req = x86::requirement(insn);
    std::printf("%016llX: %u %s%s\n", static_cast<unsigned long long>(va), insn.length,
                req.feature ? host::feature_info(*req.feature).name : "-", req.not_on_console ? " not-on-console" : "");
    i += insn.length;
  }
  return 0;
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
    if (config.game_dir.empty()) return usage();
    opt.target = config.game_dir;
  }

  std::vector<fs::path> targets;
  std::error_code ec;
  if (fs::is_directory(opt.target, ec)) {
    for (const auto& entry : fs::directory_iterator(opt.target, ec)) {
      if (entry.is_regular_file(ec) && (has_extension(entry.path(), L".exe") || has_extension(entry.path(), L".dll"))) {
        targets.push_back(entry.path());
      }
    }
    std::sort(targets.begin(), targets.end(), [](const fs::path& a, const fs::path& b) {
      const bool ae = has_extension(a, L".exe"), be = has_extension(b, L".exe");
      if (ae != be) return ae;
      return to_lower_ascii(narrow(a.filename().wstring())) < to_lower_ascii(narrow(b.filename().wstring()));
    });
  } else {
    targets.push_back(opt.target);
  }
  if (opt.boundaries && targets.size() != 1) return usage();

  const host::CpuidSnapshot cpu = host::read_cpuid();
  const std::vector<CpuFeature> locate = features_to_locate(cpu);

  JsonWriter j;
  j.begin_object();
  j.field("host_cpu", cpu.brand);
  j.key("host_lacks").begin_array();
  for (CpuFeature f : locate) j.value(host::feature_info(f).name);
  j.end_array();
  j.key("modules").begin_array();

  int failures = 0;
  x86::CodeScan::Counts total_missing{};
  for (const fs::path& path : targets) {
    std::string error;
    auto view = FileView::open(path, error);
    std::optional<pe::Image> img;
    if (view) img = pe::parse(view->bytes(), error);
    if (!img) {
      std::fprintf(stderr, "%s: %s\n", narrow(path.wstring()).c_str(), error.c_str());
      ++failures;
      continue;
    }
    if (opt.boundaries) return print_boundaries(*img, view->bytes(), opt.boundaries->first, opt.boundaries->second);

    const x86::CodeScan scan = x86::scan_code(*img, view->bytes(), locate);
    const std::string name = narrow(path.filename().wstring());
    using x86::Confidence;
    const auto C = static_cast<size_t>(Confidence::Confirmed);
    const auto F = static_cast<size_t>(Confidence::InFunction);
    const auto O = static_cast<size_t>(Confidence::Outside);
    std::printf("%s: %s of code, %llu functions in the exception table\n", name.c_str(), format_bytes(scan.bytes).c_str(),
                static_cast<unsigned long long>(scan.functions));
    std::printf("  instructions   confirmed %llu, in functions %llu, outside functions %llu\n",
                static_cast<unsigned long long>(scan.instructions[C]),
                static_cast<unsigned long long>(scan.instructions[F]),
                static_cast<unsigned long long>(scan.instructions[O]));
    std::printf("  decode errors  confirmed %llu, in functions %llu, outside %llu; overlaps %llu; padding %s\n",
                static_cast<unsigned long long>(scan.decode_errors[C]),
                static_cast<unsigned long long>(scan.decode_errors[F]),
                static_cast<unsigned long long>(scan.decode_errors[O]), static_cast<unsigned long long>(scan.overlaps),
                format_bytes(scan.padding).c_str());
    std::printf("  not-on-console encodings (data seen as code): confirmed %llu, in functions %llu, outside %llu\n",
                static_cast<unsigned long long>(scan.not_on_console[C]),
                static_cast<unsigned long long>(scan.not_on_console[F]),
                static_cast<unsigned long long>(scan.not_on_console[O]));
    std::printf("  %-16s %12s %12s %10s  %s\n", "feature", "confirmed", "in-function", "outside", "this host");
    for (const host::CpuFeatureInfo& info : host::console_cpu_features()) {
      const auto& c = scan.by_feature[static_cast<size_t>(info.feature)];
      if (!c[C] && !c[F] && !c[O]) continue;
      const bool has = host::has_feature(cpu, info.feature);
      std::printf("  %-16s %12llu %12llu %10llu  %s\n", info.name, static_cast<unsigned long long>(c[C]),
                  static_cast<unsigned long long>(c[F]), static_cast<unsigned long long>(c[O]),
                  has ? "has it" : "LACKS IT");
      if (!has) {
        total_missing[C] += c[C];
        total_missing[F] += c[F];
        total_missing[O] += c[O];
      }
    }
    if (opt.sites) {
      for (const x86::Site& s : scan.sites) {
        const auto bytes = pe::bytes_at(*img, view->bytes(), s.rva, s.length);
        std::printf("    +0x%08x %-16s %-11s %s\n", s.rva, host::feature_info(s.feature).name,
                    x86::confidence_name(s.confidence), bytes ? hex_bytes(bytes->data(), bytes->size()).c_str() : "");
      }
    }
    std::printf("\n");

    j.begin_object().field("file", name).field("code_bytes", scan.bytes).field("functions", scan.functions);
    auto counts = [&](std::string_view key, const x86::CodeScan::Counts& c) {
      j.key(key).begin_object().field("confirmed", c[C]).field("in_function", c[F]).field("outside", c[O]).end_object();
    };
    counts("instructions", scan.instructions);
    counts("decode_errors", scan.decode_errors);
    counts("not_on_console", scan.not_on_console);
    j.field("overlaps", scan.overlaps).field("padding", scan.padding);
    j.key("features").begin_object();
    for (const host::CpuFeatureInfo& info : host::console_cpu_features()) {
      const auto& c = scan.by_feature[static_cast<size_t>(info.feature)];
      if (c[C] || c[F] || c[O]) counts(info.name, c);
    }
    j.end_object();
    j.key("not_on_console_confirmed").begin_array();
    for (uint32_t rva : scan.not_on_console_confirmed) j.value(rva);
    j.end_array();
    j.key("sites").begin_array();
    for (const x86::Site& s : scan.sites) {
      const auto bytes = pe::bytes_at(*img, view->bytes(), s.rva, s.length);
      j.begin_object().field("rva", s.rva).field("length", s.length)
          .field("feature", host::feature_info(s.feature).name).field("confidence", x86::confidence_name(s.confidence))
          .field("bytes", bytes ? hex_bytes(bytes->data(), bytes->size()) : std::string()).end_object();
    }
    j.end_array();
    j.end_object();
  }
  j.end_array();
  j.end_object();

  std::printf("Instructions this host lacks, all modules: confirmed %llu, in functions %llu, outside functions %llu\n",
              static_cast<unsigned long long>(total_missing[1]), static_cast<unsigned long long>(total_missing[2]),
              static_cast<unsigned long long>(total_missing[3]));

  if (!opt.json.empty()) {
    std::ofstream out(opt.json, std::ios::binary);
    out << j.str() << '\n';
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", narrow(opt.json.wstring()).c_str());
      return 1;
    }
    std::printf("wrote %s\n", narrow(opt.json.wstring()).c_str());
  }
  return failures ? 1 : 0;
}
