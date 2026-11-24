#include "pe/pe_image.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace am::pe {
namespace {

constexpr size_t kMaxModules = 4096;
constexpr size_t kMaxThunks = 1 << 16;
constexpr uint32_t kMaxExports = 1 << 20;
constexpr size_t kMaxCallbacks = 256;
constexpr size_t kMaxDebugEntries = 64;

// Bounds-checked access to the file bytes.
class Bytes {
 public:
  explicit Bytes(std::span<const uint8_t> d) : d_(d) {}

  template <class T>
  bool read(uint64_t off, T& out) const {
    if (off > d_.size() || d_.size() - off < sizeof(T)) return false;
    std::memcpy(&out, d_.data() + off, sizeof(T));
    return true;
  }

  // The NUL-terminated string at `off` (at most `max` bytes); nullopt when out of range or unterminated.
  std::optional<std::string> cstr(uint64_t off, size_t max = 4096) const {
    if (off >= d_.size()) return std::nullopt;
    const size_t avail = std::min<size_t>(static_cast<size_t>(d_.size() - off), max);
    const char* p = reinterpret_cast<const char*>(d_.data() + off);
    const void* nul = std::memchr(p, 0, avail);
    if (!nul) return std::nullopt;
    return std::string(p, static_cast<const char*>(nul));
  }

 private:
  std::span<const uint8_t> d_;
};

void parse_thunks(Image& img, const Bytes& b, uint32_t thunk_rva, ImportedModule& mod) {
  const auto off = img.rva_to_offset(thunk_rva);
  if (!off) {
    img.warnings.push_back("import thunks of " + mod.dll + " lie outside the file");
    return;
  }
  for (size_t i = 0; i < kMaxThunks; ++i) {
    uint64_t thunk = 0;
    if (!b.read(*off + i * 8, thunk)) {
      img.warnings.push_back("import thunks of " + mod.dll + " are truncated");
      return;
    }
    if (thunk == 0) return;
    ImportedFunction f;
    if (thunk & IMAGE_ORDINAL_FLAG64) {
      f.by_ordinal = true;
      f.ordinal = static_cast<uint16_t>(thunk & 0xffff);
    } else {
      const auto hint_name = img.rva_to_offset(static_cast<uint32_t>(thunk & 0x7fffffff));
      uint16_t hint = 0;
      std::optional<std::string> name;
      if (hint_name && b.read(*hint_name, hint)) name = b.cstr(*hint_name + 2, 1024);
      if (!name) {
        img.warnings.push_back("an import name of " + mod.dll + " is unreadable");
        continue;
      }
      f.hint = hint;
      f.name = std::move(*name);
    }
    mod.functions.push_back(std::move(f));
  }
  img.warnings.push_back("import thunks of " + mod.dll + " exceed the limit");
}

void parse_imports(Image& img, const Bytes& b) {
  const DataDirectory& d = img.dir(Directory::Import);
  if (!d.rva) return;
  const auto off = img.rva_to_offset(d.rva);
  if (!off) {
    img.warnings.push_back("import directory lies outside the file");
    return;
  }
  for (size_t i = 0; i < kMaxModules; ++i) {
    IMAGE_IMPORT_DESCRIPTOR desc{};
    if (!b.read(*off + i * sizeof(desc), desc)) {
      img.warnings.push_back("import directory is truncated");
      return;
    }
    if (desc.Name == 0 && desc.FirstThunk == 0) return;
    ImportedModule mod;
    const auto name_off = img.rva_to_offset(desc.Name);
    const auto name = name_off ? b.cstr(*name_off, 512) : std::nullopt;
    mod.dll = name ? *name : "<unreadable name>";
    parse_thunks(img, b, desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk, mod);
    img.imports.push_back(std::move(mod));
  }
}

void parse_delay_imports(Image& img, const Bytes& b) {
  const DataDirectory& d = img.dir(Directory::DelayImport);
  if (!d.rva) return;
  const auto off = img.rva_to_offset(d.rva);
  if (!off) {
    img.warnings.push_back("delay-import directory lies outside the file");
    return;
  }
  for (size_t i = 0; i < kMaxModules; ++i) {
    IMAGE_DELAYLOAD_DESCRIPTOR desc{};
    if (!b.read(*off + i * sizeof(desc), desc)) {
      img.warnings.push_back("delay-import directory is truncated");
      return;
    }
    if (desc.DllNameRVA == 0) return;
    // Old descriptors hold virtual addresses instead of RVAs.
    const bool rva_based = desc.Attributes.RvaBased != 0;
    auto fix = [&](uint32_t v) { return rva_based ? v : static_cast<uint32_t>(v - img.image_base); };
    ImportedModule mod;
    mod.delay_load = true;
    const auto name_off = img.rva_to_offset(fix(desc.DllNameRVA));
    const auto name = name_off ? b.cstr(*name_off, 512) : std::nullopt;
    mod.dll = name ? *name : "<unreadable name>";
    if (desc.ImportNameTableRVA) parse_thunks(img, b, fix(desc.ImportNameTableRVA), mod);
    img.imports.push_back(std::move(mod));
  }
}

void parse_exports(Image& img, const Bytes& b) {
  const DataDirectory& d = img.dir(Directory::Export);
  if (!d.rva) return;
  const auto off = img.rva_to_offset(d.rva);
  IMAGE_EXPORT_DIRECTORY ed{};
  if (!off || !b.read(*off, ed)) {
    img.warnings.push_back("export directory is unreadable");
    return;
  }
  if (const auto name_off = img.rva_to_offset(ed.Name)) {
    if (auto name = b.cstr(*name_off, 512)) img.export_dll_name = std::move(*name);
  }
  if (ed.NumberOfFunctions > kMaxExports || ed.NumberOfNames > kMaxExports) {
    img.warnings.push_back("export table exceeds the limit");
    return;
  }
  const auto funcs = img.rva_to_offset(ed.AddressOfFunctions);
  if (!funcs && ed.NumberOfFunctions) {
    img.warnings.push_back("export address table lies outside the file");
    return;
  }
  std::vector<Export> exports(ed.NumberOfFunctions);
  for (uint32_t i = 0; i < ed.NumberOfFunctions; ++i) {
    uint32_t rva = 0;
    if (!b.read(*funcs + uint64_t{i} * 4, rva)) {
      img.warnings.push_back("export address table is truncated");
      break;
    }
    exports[i].ordinal = ed.Base + i;
    exports[i].rva = rva;
    if (rva >= d.rva && rva - d.rva < d.size) {
      if (const auto fwd = img.rva_to_offset(rva)) {
        if (auto s = b.cstr(*fwd, 512)) exports[i].forwarder = std::move(*s);
      }
    }
  }
  const auto names = img.rva_to_offset(ed.AddressOfNames);
  const auto ordinals = img.rva_to_offset(ed.AddressOfNameOrdinals);
  if (ed.NumberOfNames && (!names || !ordinals)) {
    img.warnings.push_back("export name tables lie outside the file");
  } else {
    for (uint32_t i = 0; i < ed.NumberOfNames; ++i) {
      uint32_t name_rva = 0;
      uint16_t index = 0;
      if (!b.read(*names + uint64_t{i} * 4, name_rva) || !b.read(*ordinals + uint64_t{i} * 2, index)) {
        img.warnings.push_back("export name tables are truncated");
        break;
      }
      if (index >= exports.size()) continue;
      if (const auto name_off = img.rva_to_offset(name_rva)) {
        if (auto s = b.cstr(*name_off, 1024)) exports[index].name = std::move(*s);
      }
    }
  }
  std::erase_if(exports, [](const Export& e) { return e.rva == 0; });
  img.exports = std::move(exports);
}

void parse_tls(Image& img, const Bytes& b) {
  const DataDirectory& d = img.dir(Directory::Tls);
  if (!d.rva) return;
  const auto off = img.rva_to_offset(d.rva);
  IMAGE_TLS_DIRECTORY64 td{};
  if (!off || !b.read(*off, td)) {
    img.warnings.push_back("TLS directory is unreadable");
    return;
  }
  Tls tls;
  tls.raw_data_start_va = td.StartAddressOfRawData;
  tls.raw_data_end_va = td.EndAddressOfRawData;
  tls.index_va = td.AddressOfIndex;
  tls.callbacks_va = td.AddressOfCallBacks;
  tls.zero_fill = td.SizeOfZeroFill;
  tls.characteristics = td.Characteristics;
  if (tls.raw_data_end_va < tls.raw_data_start_va) {
    img.warnings.push_back("TLS template ends before it starts");
    tls.raw_data_end_va = tls.raw_data_start_va;
  }
  if (tls.callbacks_va > img.image_base) {
    const auto cb = img.rva_to_offset(static_cast<uint32_t>(tls.callbacks_va - img.image_base));
    for (size_t i = 0; cb && i < kMaxCallbacks; ++i) {
      uint64_t va = 0;
      if (!b.read(*cb + i * 8, va) || va == 0) break;
      tls.callbacks.push_back(va);
    }
  }
  img.tls = std::move(tls);
}

void parse_debug(Image& img, const Bytes& b) {
  const DataDirectory& d = img.dir(Directory::Debug);
  if (!d.rva) return;
  const auto off = img.rva_to_offset(d.rva);
  if (!off) return;
  const size_t count = std::min<size_t>(d.size / sizeof(IMAGE_DEBUG_DIRECTORY), kMaxDebugEntries);
  for (size_t i = 0; i < count; ++i) {
    IMAGE_DEBUG_DIRECTORY dd{};
    if (!b.read(*off + i * sizeof(dd), dd)) return;
    if (dd.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
    uint32_t signature = 0;
    if (!b.read(dd.PointerToRawData, signature) || signature != 0x53445352) continue;  // "RSDS"
    CodeView cv;
    if (!b.read(uint64_t{dd.PointerToRawData} + 4, cv.guid) || !b.read(uint64_t{dd.PointerToRawData} + 20, cv.age)) {
      continue;
    }
    if (auto path = b.cstr(uint64_t{dd.PointerToRawData} + 24, 1024)) cv.pdb_path = std::move(*path);
    img.codeview = std::move(cv);
    return;
  }
}

void parse_relocations(Image& img, const Bytes& b) {
  const DataDirectory& d = img.dir(Directory::BaseReloc);
  if (!d.rva || !d.size) return;
  const auto off = img.rva_to_offset(d.rva);
  if (!off) {
    img.warnings.push_back("relocation directory lies outside the file");
    return;
  }
  const uint64_t end = *off + d.size;
  uint64_t pos = *off;
  while (pos + sizeof(IMAGE_BASE_RELOCATION) <= end) {
    IMAGE_BASE_RELOCATION block{};
    if (!b.read(pos, block)) {
      img.warnings.push_back("relocation directory is truncated");
      return;
    }
    if (block.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
      img.warnings.push_back("relocation block with a bad size");
      return;
    }
    ++img.relocation_blocks;
    img.relocation_entries += (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
    pos += block.SizeOfBlock;
  }
}

}  // namespace

std::optional<uint64_t> Image::rva_to_offset(uint32_t rva) const {
  if (rva < size_of_headers) return rva;
  for (const Section& s : sections) {
    const uint32_t extent = std::max(s.virtual_size, s.raw_size);
    if (rva >= s.rva && rva - s.rva < extent) {
      const uint32_t delta = rva - s.rva;
      if (delta >= s.raw_size) return std::nullopt;  // in the zero-filled tail, not in the file
      return uint64_t{s.raw_offset} + delta;
    }
  }
  return std::nullopt;
}

const Section* Image::section_of(uint32_t rva) const {
  for (const Section& s : sections) {
    if (rva >= s.rva && rva - s.rva < std::max(s.virtual_size, s.raw_size)) return &s;
  }
  return nullptr;
}

size_t Image::import_count() const {
  size_t n = 0;
  for (const ImportedModule& m : imports) n += m.functions.size();
  return n;
}

std::optional<Image> parse(std::span<const uint8_t> file, std::string& error) {
  const Bytes b(file);
  IMAGE_DOS_HEADER dos{};
  if (!b.read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
    error = "not an MZ executable";
    return std::nullopt;
  }
  const uint64_t nt = static_cast<uint32_t>(dos.e_lfanew);
  DWORD signature = 0;
  if (!b.read(nt, signature) || signature != IMAGE_NT_SIGNATURE) {
    error = "no PE signature";
    return std::nullopt;
  }
  IMAGE_FILE_HEADER fh{};
  if (!b.read(nt + 4, fh)) {
    error = "file header is truncated";
    return std::nullopt;
  }
  const uint64_t opt_off = nt + 4 + sizeof(IMAGE_FILE_HEADER);
  WORD magic = 0;
  if (!b.read(opt_off, magic)) {
    error = "optional header is truncated";
    return std::nullopt;
  }
  if (magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    error = "not a PE32+ (64-bit) image";
    return std::nullopt;
  }
  const size_t opt_size = fh.SizeOfOptionalHeader;
  constexpr size_t kDirsOffset = offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
  if (opt_size < kDirsOffset) {
    error = "optional header is too small";
    return std::nullopt;
  }
  // The optional header may be shorter than the structure when it has fewer data directories.
  IMAGE_OPTIONAL_HEADER64 oh{};
  const size_t copy = std::min(opt_size, sizeof(oh));
  if (opt_off > file.size() || file.size() - opt_off < copy) {
    error = "optional header is truncated";
    return std::nullopt;
  }
  std::memcpy(&oh, file.data() + opt_off, copy);

  Image img;
  img.machine = fh.Machine;
  img.file_characteristics = fh.Characteristics;
  img.timestamp = fh.TimeDateStamp;
  img.image_base = oh.ImageBase;
  img.entry_rva = oh.AddressOfEntryPoint;
  img.size_of_image = oh.SizeOfImage;
  img.size_of_headers = oh.SizeOfHeaders;
  img.section_alignment = oh.SectionAlignment;
  img.file_alignment = oh.FileAlignment;
  img.stack_reserve = oh.SizeOfStackReserve;
  img.stack_commit = oh.SizeOfStackCommit;
  img.subsystem = oh.Subsystem;
  img.dll_characteristics = oh.DllCharacteristics;

  const size_t present = (copy - kDirsOffset) / sizeof(IMAGE_DATA_DIRECTORY);
  const size_t dirs = std::min<size_t>({oh.NumberOfRvaAndSizes, present, img.directories.size()});
  for (size_t i = 0; i < dirs; ++i) {
    img.directories[i] = {oh.DataDirectory[i].VirtualAddress, oh.DataDirectory[i].Size};
  }

  const uint64_t sections_off = opt_off + fh.SizeOfOptionalHeader;
  for (WORD i = 0; i < fh.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER sh{};
    if (!b.read(sections_off + uint64_t{i} * sizeof(sh), sh)) {
      error = "section table is truncated";
      return std::nullopt;
    }
    Section s;
    const char* name = reinterpret_cast<const char*>(sh.Name);
    s.name.assign(name, strnlen(name, IMAGE_SIZEOF_SHORT_NAME));
    s.rva = sh.VirtualAddress;
    s.virtual_size = sh.Misc.VirtualSize;
    s.raw_offset = sh.PointerToRawData;
    s.raw_size = sh.SizeOfRawData;
    s.characteristics = sh.Characteristics;
    img.sections.push_back(std::move(s));
  }

  parse_imports(img, b);
  parse_delay_imports(img, b);
  parse_exports(img, b);
  parse_tls(img, b);
  parse_debug(img, b);
  parse_relocations(img, b);
  img.exception_entries = img.dir(Directory::Exception).size / static_cast<uint32_t>(sizeof(RUNTIME_FUNCTION));
  return img;
}

const char* machine_name(uint16_t machine) {
  switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return "AMD64";
    case IMAGE_FILE_MACHINE_I386: return "x86";
    case IMAGE_FILE_MACHINE_ARM64: return "ARM64";
    default: return "unknown";
  }
}

const char* subsystem_name(uint16_t subsystem) {
  switch (subsystem) {
    case IMAGE_SUBSYSTEM_NATIVE: return "native";
    case IMAGE_SUBSYSTEM_WINDOWS_GUI: return "Windows GUI";
    case IMAGE_SUBSYSTEM_WINDOWS_CUI: return "Windows console";
    default: return "other";
  }
}

std::string guid_string(const std::array<uint8_t, 16>& g) {
  // The first three fields are little-endian.
  char buf[40];
  std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2],
                g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
  return buf;
}

}  // namespace am::pe
