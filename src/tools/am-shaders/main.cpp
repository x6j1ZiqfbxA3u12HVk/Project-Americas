// am-shaders: the game's shader corpus, for the shader recompiler.
//
//   am-shaders index [<shaders.rpf>] [--out <dir>]
//       Finds every record of the shader pack, inflates it, and writes each distinct container
//       once as <out>\<sha256 prefix>.dxbc plus <out>\index.json. Default <out>:
//       <cache>\shaders\pack. The pack is only read.
//   am-shaders dump <name or hash prefix> [--out <dir>]
//       The structure of one container from the corpus: its parts, its program header, and the
//       four-character tags inside the console payload.
//   am-shaders survey [--out <dir>]
//       Every console program of the corpus through the RDNA 2 decoder: failures by reason and
//       an instruction histogram, in <cache>\shaders\survey.json.
//   am-shaders disasm <hash prefix> [--out <dir>] [--raw <dir>]
//       A listing of one container's console programs; --raw also writes their code bytes.
//   am-shaders translate <hash prefix> [--out <dir>] [--raw <hlsl dir>]
//       One container's programs through the shader translator; the HLSL goes to
//       <cache>\shaders\hlsl and is compiled with DXC.
//   am-shaders tsurvey [<programs to compile>] [--out <dir>] [--raw <hlsl dir>]
//       Every program through the translator: what translates, what holds the rest up, and DXC
//       on a sample, in <cache>\shaders\translation.json.
//   am-shaders images [--out <dir>]
//       Every image instruction of the corpus by its form and stage.
//   am-shaders precompile [all] [refresh] [--out <dir>] [--raw <hlsl dir>]
//   am-shaders serve [jobs] [--log <file>]
//       The shader service, while the game runs: what it waits for first, then the rest.
//       The programs the game's pipelines use (saved by the runtime in <cache>\shaders\runtime),
//       or with `all` every program, translated with the runtime's binding and compiled to
//       <cache>\shaders\dxil\rt<translator version>\<marker>.dxil, where the runtime loads them. Resumable.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/file_view.h"
#include "core/json.h"
#include "core/sha256.h"
#include "core/text.h"
#include "shader/container.h"
#include "shader/pack.h"
#include "shader/translate.h"
#include "survey.h"

namespace fs = std::filesystem;
using namespace am;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: am-shaders index [<shaders.rpf>] [--out <dir>]\n"
               "       am-shaders dump <name or hash prefix> [--out <dir>]\n"
               "       am-shaders survey [--out <dir>]\n"
               "       am-shaders disasm <hash prefix> [--out <dir>] [--raw <dir>]\n"
               "       am-shaders translate <hash prefix> [--out <dir>] [--raw <hlsl dir>]\n"
               "       am-shaders tsurvey [<programs to compile>] [--out <dir>] [--raw <hlsl dir>]\n"
               "       am-shaders images [--out <dir>]\n"
               "       am-shaders precompile [all] [refresh] [--out <dir>] [--raw <hlsl dir>]\n"
               "       am-shaders serve [jobs]\n");
  return 2;
}

struct Entry {
  std::string name;
  size_t offset = 0;
  uint32_t size = 0, packed = 0, unknown = 0;
  uint8_t flags = 0;
  std::string hash;  // 32 hex digits
  std::string kind;
  std::string model;
  std::string parts;
  std::string payload_tag;
};

std::string describe_parts(const shader::Container& c) {
  std::string s;
  for (const shader::Part& p : c.parts) s += (s.empty() ? "" : " ") + p.fourcc;
  return s;
}

int index_pack(const fs::path& pack_path, const fs::path& out) {
  std::string error;
  auto view = FileView::open(pack_path, error);
  if (!view) {
    std::fprintf(stderr, "%s: %s\n", narrow(pack_path.wstring()).c_str(), error.c_str());
    return 1;
  }
  std::error_code ec;
  fs::create_directories(out, ec);
  const auto start = std::chrono::steady_clock::now();

  std::vector<Entry> entries;
  std::set<std::string> written;
  uint64_t bytes = 0, new_bytes = 0;
  size_t bad_containers = 0;
  shader::PackScanStats stats;
  shader::scan_pack(
      view->bytes(),
      [&](const shader::PackRecord& r, std::span<const uint8_t> container) {
        Entry e;
        e.name = r.name;
        e.offset = r.offset;
        e.size = r.size;
        e.packed = r.packed_size;
        e.unknown = r.unknown;
        e.flags = r.flags;
        const auto digest = sha256(container);
        e.hash = digest ? hex_bytes(digest->data(), 16) : std::string("nohash");
        std::string err;
        if (const auto c = shader::parse_container(container, err)) {
          e.parts = describe_parts(*c);
          if (const shader::Part* dxil = c->find("DXIL")) {
            if (const auto h = shader::read_program_header(dxil->data)) {
              e.kind = shader::program_kind_name(h->kind());
              e.model = std::to_string(h->major()) + "." + std::to_string(h->minor());
              e.payload_tag = shader::fourcc_at(dxil->data, 24);
            }
          }
        } else {
          ++bad_containers;
        }
        bytes += container.size();
        if (written.insert(e.hash).second) {
          const fs::path file = out / widen(e.hash + ".dxbc");
          if (!fs::exists(file, ec)) {
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(container.data()),
                                                         static_cast<std::streamsize>(container.size()));
            new_bytes += container.size();
          }
        }
        entries.push_back(std::move(e));
        if (entries.size() % 2000 == 0) {
          std::printf("  %zu records...\n", entries.size());
          std::fflush(stdout);
        }
      },
      stats);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::map<std::string, size_t> by_kind, by_tag, by_parts;
  for (const Entry& e : entries) {
    ++by_kind[e.kind.empty() ? "(no DXIL part)" : e.kind];
    ++by_tag[e.payload_tag.empty() ? "(none)" : e.payload_tag];
    ++by_parts[e.parts];
  }

  JsonWriter j;
  j.begin_object();
  j.field("pack", narrow(pack_path.wstring())).field("records", entries.size()).field("distinct", written.size());
  j.key("by_kind").begin_object();
  for (const auto& [k, n] : by_kind) j.field(k, n);
  j.end_object();
  j.key("entries").begin_array();
  for (const Entry& e : entries) {
    j.begin_object().field("name", e.name).field("offset", e.offset).field("size", e.size).field("packed", e.packed)
        .field("flags", e.flags).field("unknown", e.unknown).field("hash", e.hash).field("kind", e.kind)
        .field("model", e.model).field("parts", e.parts).field("payload", e.payload_tag).end_object();
  }
  j.end_array();
  j.end_object();
  std::ofstream(out / L"index.json", std::ios::binary) << j.str() << '\n';

  std::printf("%zu records (%zu distinct containers, %s inflated, %s newly written) in %.1f s\n", entries.size(),
              written.size(), format_bytes(bytes).c_str(), format_bytes(new_bytes).c_str(), seconds);
  std::printf("framed like records but rejected: %zu; unreadable containers: %zu\n", stats.rejected, bad_containers);
  std::printf("by program kind:\n");
  for (const auto& [k, n] : by_kind) std::printf("  %-16s %6zu\n", k.c_str(), n);
  std::printf("console payload tag:\n");
  for (const auto& [k, n] : by_tag) std::printf("  %-16s %6zu\n", k.c_str(), n);
  std::printf("part layouts:\n");
  std::vector<std::pair<size_t, std::string>> layouts;
  for (const auto& [k, n] : by_parts) layouts.emplace_back(n, k);
  std::sort(layouts.rbegin(), layouts.rend());
  for (size_t i = 0; i < layouts.size() && i < 12; ++i) std::printf("  %6zu  %s\n", layouts[i].first, layouts[i].second.c_str());
  std::printf("wrote %s\n", narrow((out / L"index.json").wstring()).c_str());
  return 0;
}

int dump(const std::string& wanted, const fs::path& out) {
  std::ifstream in(out / L"index.json", std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "no index in %s: run am-shaders index first\n", narrow(out.wstring()).c_str());
    return 1;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // Find the entry: by hash prefix, else by name substring (the index is ours: a plain search will do).
  std::string hash, name;
  for (size_t p = text.find("\"name\": \""); p != std::string::npos; p = text.find("\"name\": \"", p + 1)) {
    const size_t ns = p + 9, ne = text.find('"', ns);
    const std::string n = text.substr(ns, ne - ns);
    const size_t hp = text.find("\"hash\": \"", ne);
    const std::string h = text.substr(hp + 9, 32);
    if (istarts_with_ascii(h, wanted) || n.find(wanted) != std::string::npos) {
      hash = h;
      name = n;
      break;
    }
  }
  if (hash.empty()) {
    std::fprintf(stderr, "no record matches %s\n", wanted.c_str());
    return 1;
  }
  std::string error;
  auto view = FileView::open(out / widen(hash + ".dxbc"), error);
  if (!view) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const auto blob = view->bytes();
  const auto c = shader::parse_container(blob, error);
  if (!c) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::printf("%s (%s), %zu bytes, %zu parts\n", name.c_str(), hash.c_str(), blob.size(), c->parts.size());
  for (const shader::Part& p : c->parts) std::printf("  %s at +0x%x, %zu bytes\n", p.fourcc.c_str(), p.offset, p.data.size());
  if (const shader::Part* dxil = c->find("DXIL")) {
    if (const auto h = shader::read_program_header(dxil->data)) {
      std::printf("program: %s %u.%u, %u dwords, DXIL version 0x%x, payload at +0x%x (from the magic), %u bytes\n",
                  shader::program_kind_name(h->kind()), h->major(), h->minor(), h->size_dwords, h->dxil_version,
                  h->payload_offset, h->payload_size);
      // Tags inside the console payload: four printable capitals at 4-byte alignment.
      std::printf("tags in the DXIL part:\n");
      for (size_t p = 24; p + 8 <= dxil->data.size(); p += 4) {
        const std::string t = shader::fourcc_at(dxil->data, p);
        if (t.size() == 4 && std::all_of(t.begin(), t.end(), [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); })) {
          uint32_t next;
          std::memcpy(&next, dxil->data.data() + p + 4, 4);
          std::printf("  +0x%04zx %s  next u32 0x%x\n", p, t.c_str(), next);
        }
      }
    }
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return usage();
  const std::wstring command = argv[1];
  std::vector<std::string> warnings;
  const Config config = load_config(warnings);
  fs::path out = config.cache_dir() / L"shaders" / L"pack";
  std::vector<std::wstring> positional;
  fs::path raw, log;  // --log: serve appends its output there too
  for (int i = 2; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--out" && i + 1 < argc) out = argv[++i];
    else if (a == L"--raw" && i + 1 < argc) raw = argv[++i];
    else if (a == L"--log" && i + 1 < argc) log = argv[++i];
    else if (!a.empty() && a[0] != L'-') positional.push_back(a);
    else return usage();
  }
  if (command == L"index") {
    fs::path pack = positional.empty() ? config.game_dir / L"shaders.rpf" : fs::path(positional[0]);
    if (pack.empty() || positional.size() > 1) return usage();
    return index_pack(pack, out);
  }
  if (command == L"dump" && positional.size() == 1) return dump(narrow(positional[0]), out);
  if (command == L"survey" && positional.empty()) return tools::survey(out, out.parent_path() / L"survey.json");
  if (command == L"disasm" && positional.size() == 1) return tools::disasm(narrow(positional[0]), out, raw);
  if (command == L"translate" && positional.size() == 1) {
    return tools::translate(narrow(positional[0]), out, raw.empty() ? out.parent_path() / L"hlsl" : raw);
  }
  if (command == L"images" && positional.empty()) return tools::image_survey(out);
  if (command == L"precompile" && positional.size() <= 2) {
    const bool all = std::find(positional.begin(), positional.end(), L"all") != positional.end();
    const bool refresh = std::find(positional.begin(), positional.end(), L"refresh") != positional.end();
    // One folder a translator version, as the runtime loads them (host_pipeline.cpp).
    return tools::precompile(out, out.parent_path() / L"runtime",
                             out.parent_path() / L"dxil" / (L"rt" + std::to_wstring(shader::kTranslatorVersion)), all, raw,
                             refresh);
  }
  if (command == L"guard-loops" && positional.size() <= 1) {
    const bool apply = !positional.empty() && positional[0] == L"apply";
    return tools::invalidate_loops(out, out.parent_path() / L"runtime",
                                   out.parent_path() / L"dxil" / (L"rt" + std::to_wstring(shader::kTranslatorVersion)), apply);
  }
  if (command == L"serve" && positional.size() <= 1) {
    const unsigned jobs = positional.empty() ? std::max(1u, std::thread::hardware_concurrency() - 2) : std::stoul(positional[0]);
    return tools::serve(out, out.parent_path() / L"runtime",
                        out.parent_path() / L"dxil" / (L"rt" + std::to_wstring(shader::kTranslatorVersion)), jobs, log);
  }
  if (command == L"tsurvey" && positional.size() <= 1) {
    const size_t sample = positional.empty() ? 200 : std::stoul(positional[0]);
    return tools::translation_survey(out, out.parent_path() / L"translation.json", sample, raw);
  }
  return usage();
}
