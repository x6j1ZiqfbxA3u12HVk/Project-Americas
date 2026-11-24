#pragma once

// A title module (the executable or one of its DLLs) mapped into our process: sections copied
// from the read-only file, relocated, later bound, protected and registered for unwinding.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

#include "pe/pe_image.h"

namespace am::rt {

struct MappedImage {
  std::string name;            // file name, as imports refer to it
  std::filesystem::path path;  // the source file (only ever read)
  pe::Image info;              // headers as parsed from the file
  uint8_t* base = nullptr;
  size_t size = 0;
  bool relocated = false;
  bool initialized = false;
  bool thread_calls = true;  // DLL_THREAD_ATTACH / DETACH wanted (DisableThreadLibraryCalls clears it)

  bool contains(const void* p) const {
    const auto a = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(base);
    return a >= b && a - b < size;
  }
  template <class T>
  T* at(uint32_t rva) const {
    return reinterpret_cast<T*>(base + rva);
  }

  // An export: its address, or the "module.function" it forwards to.
  using Export = std::variant<std::monostate, void*, std::string>;
  Export find_export(std::string_view wanted) const;
  Export find_export_ordinal(uint32_t ordinal) const;
};

struct MapOptions {
  bool force_relocation = false;  // map somewhere other than the preferred base (tests)
};

// Maps the file: headers and sections copied, relocations applied. Imports are not bound and
// pages stay writable until finish().
std::optional<MappedImage> map_image(const std::filesystem::path& path, const MapOptions& options, std::string& error);

// Sets each section's page protection and registers the exception table for unwinding.
bool finish_image(MappedImage& image, std::string& error);

}  // namespace am::rt
