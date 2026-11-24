#include "runtime/tls.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <cstring>

#include "core/text.h"
#include "runtime/log.h"

namespace am::rt {
namespace {

// The TLS directory of a module the Windows loader loaded.
const IMAGE_TLS_DIRECTORY64* loaded_tls_directory(HMODULE module) {
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if (!d.VirtualAddress) return nullptr;
  return reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + d.VirtualAddress);
}

bool add_carrier(std::vector<TlsCarrier>& out, HMODULE module, const std::string& name) {
  const IMAGE_TLS_DIRECTORY64* tls = loaded_tls_directory(module);
  if (!tls || !tls->AddressOfIndex) return false;
  TlsCarrier c;
  c.module = name;
  c.index = *reinterpret_cast<const ULONG*>(tls->AddressOfIndex);
  c.tmpl = reinterpret_cast<uint8_t*>(tls->StartAddressOfRawData);
  c.capacity = static_cast<size_t>(tls->EndAddressOfRawData - tls->StartAddressOfRawData) + tls->SizeOfZeroFill;
  out.push_back(c);
  return true;
}

using NtQueryInformationThreadFn = NTSTATUS(NTAPI*)(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);

struct ThreadBasicInformation {
  NTSTATUS exit_status;
  PVOID teb_base;
  CLIENT_ID client_id;
  KAFFINITY affinity;
  LONG priority;
  LONG base_priority;
};

// Fills slot `index` of every existing thread of this process with the template.
void fill_existing_threads(uint32_t index, const uint8_t* tmpl, size_t template_size, size_t block_size) {
  auto query = reinterpret_cast<NtQueryInformationThreadFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  if (!query) return;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof te;
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!thread) continue;
    ThreadBasicInformation tbi{};
    if (query(thread, static_cast<THREADINFOCLASS>(0), &tbi, sizeof tbi, nullptr) >= 0 && tbi.teb_base) {
      // TEB.ThreadLocalStoragePointer is at offset 0x58 on x64.
      auto** slots = *reinterpret_cast<uint8_t***>(static_cast<uint8_t*>(tbi.teb_base) + 0x58);
      if (slots && slots[index]) {
        std::memcpy(slots[index], tmpl, template_size);
        std::memset(slots[index] + template_size, 0, block_size - template_size);
      }
    }
    CloseHandle(thread);
  }
  CloseHandle(snap);
}

}  // namespace

bool TlsManager::discover(std::string& error) {
  carriers_.clear();
  if (!add_carrier(carriers_, GetModuleHandleW(nullptr), "launcher") || carriers_[0].index != 0) {
    error = "the launcher has no TLS block in slot 0";
    return false;
  }
  for (int n = 1; n <= 8; ++n) {
    const std::string name = "am_tls_" + std::to_string(n) + ".dll";
    if (HMODULE m = GetModuleHandleW(widen(name).c_str())) add_carrier(carriers_, m, name);
  }
  return true;
}

bool TlsManager::install(MappedImage& image, bool is_executable, std::string& error) {
  if (!image.info.tls) return true;
  const pe::Tls& tls = *image.info.tls;
  const size_t template_size = static_cast<size_t>(tls.template_size());
  const size_t block_size = template_size + tls.zero_fill;

  TlsCarrier* carrier = nullptr;
  if (is_executable) {
    carrier = &carriers_[0];
  } else {
    for (size_t i = 1; i < carriers_.size(); ++i) {
      if (!carriers_[i].used && carriers_[i].capacity >= block_size) {
        carrier = &carriers_[i];
        break;
      }
    }
  }
  if (!carrier || carrier->used || carrier->capacity < block_size) {
    error = "no TLS carrier with room for " + std::to_string(block_size) + " bytes (" + image.name + ")";
    return false;
  }

  // The template, as the title's image holds it (VAs relocated with the image).
  const int64_t delta = reinterpret_cast<int64_t>(image.base) - static_cast<int64_t>(image.info.image_base);
  const auto* tmpl = reinterpret_cast<const uint8_t*>(static_cast<int64_t>(tls.raw_data_start_va) + delta);
  auto* index_var = reinterpret_cast<ULONG*>(static_cast<int64_t>(tls.index_va) + delta);
  if (!image.contains(tmpl) || (template_size && !image.contains(tmpl + template_size - 1)) || !image.contains(index_var)) {
    error = "TLS directory of " + image.name + " points outside the image";
    return false;
  }

  DWORD old = 0;
  if (!VirtualProtect(carrier->tmpl, carrier->capacity, PAGE_READWRITE, &old)) {
    error = "cannot write the TLS template of " + carrier->module;
    return false;
  }
  std::memcpy(carrier->tmpl, tmpl, template_size);
  std::memset(carrier->tmpl + template_size, 0, carrier->capacity - template_size);
  VirtualProtect(carrier->tmpl, carrier->capacity, old, &old);

  fill_existing_threads(carrier->index, tmpl, template_size, carrier->capacity);
  *index_var = carrier->index;
  carrier->used = true;
  logf("tls: %s uses slot %u of %s (%zu of %zu bytes)", image.name.c_str(), carrier->index, carrier->module.c_str(),
       block_size, carrier->capacity);
  return true;
}

}  // namespace am::rt
