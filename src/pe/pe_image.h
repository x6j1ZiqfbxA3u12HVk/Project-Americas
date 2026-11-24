#pragma once

// Read-only analysis of PE32+ images from their file bytes. Nothing here maps or runs code.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace am::pe {

enum class Directory : int {
  Export = 0,
  Import = 1,
  Resource = 2,
  Exception = 3,
  Security = 4,
  BaseReloc = 5,
  Debug = 6,
  Architecture = 7,
  GlobalPtr = 8,
  Tls = 9,
  LoadConfig = 10,
  BoundImport = 11,
  Iat = 12,
  DelayImport = 13,
  ComDescriptor = 14,
};

struct DataDirectory {
  uint32_t rva = 0;
  uint32_t size = 0;
};

struct Section {
  std::string name;
  uint32_t rva = 0;
  uint32_t virtual_size = 0;
  uint32_t raw_offset = 0;
  uint32_t raw_size = 0;
  uint32_t characteristics = 0;
};

struct ImportedFunction {
  std::string name;      // empty when imported by ordinal
  uint16_t ordinal = 0;  // valid when by_ordinal
  uint16_t hint = 0;
  bool by_ordinal = false;
};

struct ImportedModule {
  std::string dll;
  bool delay_load = false;
  std::vector<ImportedFunction> functions;
};

struct Export {
  std::string name;  // empty for exports by ordinal only
  uint32_t ordinal = 0;
  uint32_t rva = 0;
  std::string forwarder;  // "module.function" when the export forwards elsewhere
};

struct Tls {
  uint64_t raw_data_start_va = 0;
  uint64_t raw_data_end_va = 0;
  uint64_t index_va = 0;
  uint64_t callbacks_va = 0;
  uint32_t zero_fill = 0;
  uint32_t characteristics = 0;
  std::vector<uint64_t> callbacks;  // virtual addresses

  uint64_t template_size() const { return raw_data_end_va - raw_data_start_va; }
  uint64_t block_size() const { return template_size() + zero_fill; }
};

struct CodeView {
  std::array<uint8_t, 16> guid{};
  uint32_t age = 0;
  std::string pdb_path;
};

struct Image {
  uint16_t machine = 0;
  uint16_t file_characteristics = 0;
  uint32_t timestamp = 0;
  uint64_t image_base = 0;
  uint32_t entry_rva = 0;
  uint32_t size_of_image = 0;
  uint32_t size_of_headers = 0;
  uint32_t section_alignment = 0;
  uint32_t file_alignment = 0;
  uint64_t stack_reserve = 0;
  uint64_t stack_commit = 0;
  uint16_t subsystem = 0;
  uint16_t dll_characteristics = 0;
  std::array<DataDirectory, 16> directories{};
  std::vector<Section> sections;
  std::vector<ImportedModule> imports;  // regular imports first, then delay-load imports
  std::string export_dll_name;
  std::vector<Export> exports;
  std::optional<Tls> tls;
  std::optional<CodeView> codeview;
  uint32_t exception_entries = 0;
  uint32_t relocation_blocks = 0;
  uint32_t relocation_entries = 0;
  std::vector<std::string> warnings;  // problems found in tables; the headers themselves were sound

  bool is_dll() const { return (file_characteristics & 0x2000) != 0; }
  const DataDirectory& dir(Directory d) const { return directories[static_cast<int>(d)]; }
  std::optional<uint64_t> rva_to_offset(uint32_t rva) const;
  const Section* section_of(uint32_t rva) const;
  size_t import_count() const;
};

// Parses a PE32+ image. Never reads outside `file`. Broken headers give an error; broken tables
// give an image with what could be read and a note in `warnings`.
std::optional<Image> parse(std::span<const uint8_t> file, std::string& error);

const char* machine_name(uint16_t machine);
const char* subsystem_name(uint16_t subsystem);
std::string guid_string(const std::array<uint8_t, 16>& guid);

}  // namespace am::pe
