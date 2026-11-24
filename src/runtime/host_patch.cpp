#include "runtime/host_patch.h"

#include <psapi.h>

#include <vector>

namespace am::rt {

size_t patch_module_imports(HMODULE module, const void* original, const void* replacement) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!d.VirtualAddress) return 0;
  size_t patched = 0;
  for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + d.VirtualAddress); desc->Name; ++desc) {
    auto* slot = reinterpret_cast<uint64_t*>(base + desc->FirstThunk);
    for (; *slot; ++slot) {
      if (*slot != reinterpret_cast<uint64_t>(original)) continue;
      DWORD old = 0;
      if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) continue;
      *slot = reinterpret_cast<uint64_t>(replacement);
      VirtualProtect(slot, sizeof *slot, old, &old);
      ++patched;
    }
  }
  return patched;
}

size_t patch_loaded_imports(const void* original, const void* replacement) {
  std::vector<HMODULE> modules(1024);
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                          &needed)) {
    return 0;
  }
  modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
  size_t patched = 0;
  for (HMODULE m : modules) patched += patch_module_imports(m, original, replacement);
  return patched;
}

}  // namespace am::rt
