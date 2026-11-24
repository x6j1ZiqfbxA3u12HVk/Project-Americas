#include "runtime/image.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/file_view.h"
#include "core/text.h"
#include "runtime/log.h"
#include "runtime/memory/title_memory.h"

namespace am::rt {
namespace {

bool apply_relocations(MappedImage& m, int64_t delta, std::string& error) {
  const pe::DataDirectory& d = m.info.dir(pe::Directory::BaseReloc);
  if (!d.rva || !d.size) {
    error = "the image must move but has no relocations";
    return false;
  }
  uint32_t pos = d.rva;
  const uint32_t end = d.rva + d.size;
  while (pos + sizeof(IMAGE_BASE_RELOCATION) <= end) {
    const auto* block = m.at<IMAGE_BASE_RELOCATION>(pos);
    if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || pos + block->SizeOfBlock > end) {
      error = "malformed relocation block";
      return false;
    }
    const auto* entries = reinterpret_cast<const uint16_t*>(block + 1);
    const size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
    for (size_t i = 0; i < count; ++i) {
      const uint16_t type = entries[i] >> 12;
      const uint32_t rva = block->VirtualAddress + (entries[i] & 0xFFF);
      if (rva + 8 > m.size && type == IMAGE_REL_BASED_DIR64) {
        error = "relocation outside the image";
        return false;
      }
      switch (type) {
        case IMAGE_REL_BASED_ABSOLUTE: break;
        case IMAGE_REL_BASED_DIR64: *m.at<uint64_t>(rva) += static_cast<uint64_t>(delta); break;
        case IMAGE_REL_BASED_HIGHLOW: *m.at<uint32_t>(rva) += static_cast<uint32_t>(delta); break;
        case IMAGE_REL_BASED_HIGH: *m.at<uint16_t>(rva) += static_cast<uint16_t>(static_cast<uint64_t>(delta) >> 16); break;
        case IMAGE_REL_BASED_LOW: *m.at<uint16_t>(rva) += static_cast<uint16_t>(delta); break;
        default:
          error = "unsupported relocation type " + std::to_string(type);
          return false;
      }
    }
    pos += block->SizeOfBlock;
  }
  return true;
}

DWORD protection_for(uint32_t ch) {
  const bool x = ch & IMAGE_SCN_MEM_EXECUTE, r = ch & IMAGE_SCN_MEM_READ, w = ch & IMAGE_SCN_MEM_WRITE;
  if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
  if (w) return PAGE_READWRITE;
  return r ? PAGE_READONLY : PAGE_NOACCESS;
}

}  // namespace

MappedImage::Export MappedImage::find_export(std::string_view wanted) const {
  const pe::DataDirectory& d = info.dir(pe::Directory::Export);
  if (!d.rva) return {};
  const auto* ed = at<IMAGE_EXPORT_DIRECTORY>(d.rva);
  const auto* names = at<uint32_t>(ed->AddressOfNames);
  const auto* ordinals = at<uint16_t>(ed->AddressOfNameOrdinals);
  // Export names are sorted: binary search.
  size_t lo = 0, hi = ed->NumberOfNames;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const std::string_view candidate(at<const char>(names[mid]));
    if (candidate == wanted) return find_export_ordinal(ed->Base + ordinals[mid]);
    if (candidate < wanted) lo = mid + 1; else hi = mid;
  }
  // Fall back to a linear scan in case a linker left the table unsorted.
  for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
    if (std::string_view(at<const char>(names[i])) == wanted) return find_export_ordinal(ed->Base + ordinals[i]);
  }
  return {};
}

MappedImage::Export MappedImage::find_export_ordinal(uint32_t ordinal) const {
  const pe::DataDirectory& d = info.dir(pe::Directory::Export);
  if (!d.rva) return {};
  const auto* ed = at<IMAGE_EXPORT_DIRECTORY>(d.rva);
  if (ordinal < ed->Base || ordinal - ed->Base >= ed->NumberOfFunctions) return {};
  const uint32_t rva = at<uint32_t>(ed->AddressOfFunctions)[ordinal - ed->Base];
  if (!rva) return {};
  if (rva >= d.rva && rva - d.rva < d.size) return std::string(at<const char>(rva));
  return static_cast<void*>(base + rva);
}

std::optional<MappedImage> map_image(const std::filesystem::path& path, const MapOptions& options, std::string& error) {
  auto view = FileView::open(path, error);
  if (!view) return std::nullopt;
  auto parsed = pe::parse(view->bytes(), error);
  if (!parsed) return std::nullopt;
  if (parsed->machine != IMAGE_FILE_MACHINE_AMD64) {
    error = "not an x86-64 image";
    return std::nullopt;
  }

  MappedImage m;
  m.name = narrow(path.filename().wstring());
  m.path = path;
  m.info = std::move(*parsed);
  m.size = m.info.size_of_image;

  void* want = options.force_relocation ? nullptr : reinterpret_cast<void*>(m.info.image_base);
  void* block = nullptr;
  if (want) block = VirtualAlloc(want, m.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (want && !block) {
    // Say what holds the preferred range: the first allocation in it.
    const DWORD why = GetLastError();
    MEMORY_BASIC_INFORMATION mbi{};
    uint64_t p = m.info.image_base;
    while (p < m.info.image_base + m.size && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) == sizeof mbi &&
           mbi.State == MEM_FREE) {
      p = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    rt::logf("%s: preferred base 0x%s taken (error %lu): allocation at 0x%s, type 0x%lx", m.name.c_str(),
             hex(m.info.image_base).c_str(), why, hex(reinterpret_cast<uint64_t>(mbi.AllocationBase)).c_str(), mbi.Type);
  }
  if (!block) {
    if (!m.info.dir(pe::Directory::BaseReloc).size) {
      error = "the preferred base 0x" + hex(m.info.image_base) + " is taken and the image has no relocations";
      return std::nullopt;
    }
    // Moved images stay in title memory, below the GPU's 2^40 address width like everything the
    // title may hand the GPU.
    block = mem::allocate_title_range(m.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  }
  if (!block) {
    error = "cannot allocate " + std::to_string(m.size) + " bytes: " + win32_error_text(GetLastError());
    return std::nullopt;
  }
  m.base = static_cast<uint8_t*>(block);

  const auto file = view->bytes();
  std::memcpy(m.base, file.data(), std::min<size_t>(m.info.size_of_headers, file.size()));
  for (const pe::Section& s : m.info.sections) {
    const uint32_t extent = s.virtual_size ? s.virtual_size : s.raw_size;
    if (uint64_t{s.rva} + extent > m.size) {
      error = "section " + s.name + " lies outside the image";
      VirtualFree(m.base, 0, MEM_RELEASE);
      return std::nullopt;
    }
    const uint32_t copy = std::min(s.raw_size, extent);
    if (copy && uint64_t{s.raw_offset} + copy <= file.size()) std::memcpy(m.base + s.rva, file.data() + s.raw_offset, copy);
  }

  const int64_t delta = static_cast<int64_t>(reinterpret_cast<uint64_t>(m.base) - m.info.image_base);
  if (delta != 0) {
    m.relocated = true;
    if (!apply_relocations(m, delta, error)) {
      VirtualFree(m.base, 0, MEM_RELEASE);
      return std::nullopt;
    }
    // Code that reads its own header sees where it really is.
    auto* dos = m.at<IMAGE_DOS_HEADER>(0);
    auto* nt = m.at<IMAGE_NT_HEADERS64>(static_cast<uint32_t>(dos->e_lfanew));
    nt->OptionalHeader.ImageBase = reinterpret_cast<uint64_t>(m.base);
  }
  return m;
}

bool finish_image(MappedImage& m, std::string& error) {
  DWORD old = 0;
  if (!VirtualProtect(m.base, std::max<uint32_t>(m.info.size_of_headers, 1), PAGE_READONLY, &old)) {
    error = "cannot protect the headers: " + win32_error_text(GetLastError());
    return false;
  }
  for (const pe::Section& s : m.info.sections) {
    const uint32_t extent = s.virtual_size ? s.virtual_size : s.raw_size;
    if (!extent) continue;
    if (!VirtualProtect(m.base + s.rva, extent, protection_for(s.characteristics), &old)) {
      error = "cannot protect " + s.name + ": " + win32_error_text(GetLastError());
      return false;
    }
  }
  const pe::DataDirectory& ex = m.info.dir(pe::Directory::Exception);
  if (ex.rva && ex.size) {
    auto* table = m.at<RUNTIME_FUNCTION>(ex.rva);
    const auto count = static_cast<DWORD>(ex.size / sizeof(RUNTIME_FUNCTION));
    if (!RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(m.base))) {
      error = "cannot register the exception table";
      return false;
    }
  }
  FlushInstructionCache(GetCurrentProcess(), m.base, m.size);
  return true;
}

}  // namespace am::rt
