#include <windows.h>

#include <algorithm>
#include <random>
#include <vector>

#include "core/file_view.h"
#include "core/text.h"
#include "pe/pe_image.h"
#include "test.h"

using namespace am;

namespace {

std::optional<FileView> open_own_exe() {
  wchar_t path[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  std::string error;
  return FileView::open(path, error);
}

std::optional<FileView> open_system_dll(const wchar_t* name) {
  wchar_t dir[MAX_PATH];
  GetSystemDirectoryW(dir, MAX_PATH);
  std::string error;
  return FileView::open(std::filesystem::path(dir) / name, error);
}

}  // namespace

TEST_CASE(pe_parses_our_own_executable) {
  auto view = open_own_exe();
  CHECK(view.has_value());
  if (!view) return;
  std::string error;
  auto img = pe::parse(view->bytes(), error);
  CHECK(img.has_value());
  if (!img) return;
  CHECK(img->machine == IMAGE_FILE_MACHINE_AMD64);
  CHECK(!img->is_dll());
  CHECK(img->entry_rva != 0);
  CHECK(!img->sections.empty());
  CHECK(img->exception_entries > 0);
  CHECK(img->warnings.empty());
  const bool kernel32 = std::any_of(img->imports.begin(), img->imports.end(),
                                    [](const pe::ImportedModule& m) { return iequals_ascii(m.dll, "KERNEL32.dll"); });
  CHECK(kernel32);
  CHECK(img->rva_to_offset(0x10) == 0x10u);  // inside the headers
}

TEST_CASE(pe_reads_exports_and_forwarders_of_kernel32) {
  auto view = open_system_dll(L"kernel32.dll");
  CHECK(view.has_value());
  if (!view) return;
  std::string error;
  auto img = pe::parse(view->bytes(), error);
  CHECK(img.has_value());
  if (!img) return;
  CHECK(img->is_dll());
  CHECK(iequals_ascii(img->export_dll_name, "KERNEL32.dll"));
  CHECK(img->exports.size() > 1000);
  const auto has = [&](const char* name) {
    return std::any_of(img->exports.begin(), img->exports.end(), [&](const pe::Export& e) { return e.name == name; });
  };
  CHECK(has("CreateFileW"));
  const bool forwarders = std::any_of(img->exports.begin(), img->exports.end(),
                                      [](const pe::Export& e) { return !e.forwarder.empty(); });
  CHECK(forwarders);
  CHECK(img->codeview.has_value());
}

TEST_CASE(pe_survives_truncated_and_corrupted_input) {
  auto view = open_own_exe();
  CHECK(view.has_value());
  if (!view) return;
  const auto bytes = view->bytes();
  std::string error;
  CHECK(!pe::parse(bytes.first(0), error).has_value());
  CHECK(!pe::parse(bytes.first(2), error).has_value());
  CHECK(!pe::parse(bytes.first(64), error).has_value());
  // Every prefix length class must parse or fail cleanly; none may read past its end.
  for (size_t n : {size_t{200}, size_t{300}, size_t{512}, size_t{1024}, size_t{4096}, bytes.size() / 3,
                   bytes.size() / 2, bytes.size() - 1}) {
    std::vector<uint8_t> copy(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min(n, bytes.size())));
    (void)pe::parse(copy, error);
  }
  // Random byte flips anywhere in a copy.
  std::mt19937 rng(1234);
  std::vector<uint8_t> copy(bytes.begin(), bytes.end());
  for (int round = 0; round < 200; ++round) {
    std::vector<uint8_t> damaged = copy;
    for (int k = 0; k < 16; ++k) damaged[rng() % damaged.size()] = static_cast<uint8_t>(rng());
    // Hit the headers often: that is where offsets come from.
    for (int k = 0; k < 4; ++k) damaged[rng() % 1024] = static_cast<uint8_t>(rng());
    (void)pe::parse(damaged, error);
  }
  CHECK(true);
}
