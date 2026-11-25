#pragma once

// Running console code on a CPU without some of the console's instruction sets.
//
// At load, the census (x86/code_scan.h) finds the image's uses of the Zen 2-only instructions;
// its result is cached per build of the image. Uses this CPU lacks that have a same-length
// equivalent are patched in the mapped image (never on disk):
//   CLWB -> NOP, MONITORX -> NOP, MWAITX -> PAUSE, MOVNTSS / MOVNTSD -> MOVSS / MOVSD.
// The game's memory-clearing loops (`clzero; add rax, 64; cmp rax, reg; jb loop`) jump to a
// native stub next to the image that clears the line with plain stores.
// Everything else (other CLZEROs, EXTRQ / INSERTQ, RDPID, RDPRU, and any use the census could
// not confirm) raises an illegal-instruction exception when it runs; a vectored handler emulates
// it and continues after it.
//
// A CPU that has every console instruction set gets no scan, no patch and no cost.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "host/cpu.h"
#include "runtime/image.h"

namespace am::rt {

struct CpuCompatResult {
  size_t patched = 0;           // sites rewritten in place
  size_t emulated = 0;          // sites left to the exception handler
  bool from_cache = false;
  std::array<uint64_t, host::kCpuFeatureCount> uses{};  // confirmed uses per feature
};

// Patches `image` (mapped, not yet protected) for this CPU. Fails when the image needs an
// instruction set this CPU lacks that cannot be patched or emulated (AVX2 on an old CPU, say),
// unless `allow_missing` is set.
bool apply_cpu_compat(MappedImage& image, const std::filesystem::path& cache_dir, bool allow_missing,
                      CpuCompatResult& result, std::string& error);

// Rewrites the CLZERO clearing loop at `rva` to use a native stub (what apply_cpu_compat does
// for each; exposed for tests). False when no such loop starts there.
bool patch_clzero_loop_at(MappedImage& image, uint32_t rva);

// Installs the emulating exception handler (once per process).
void install_cpu_emulation();
// Whether an address is in the title's own code (set by the process; breakpoints there are the
// title's soft asserts).
void set_title_code_test(bool (*is_title_code)(const void* address));

// How often emulation ran, per instruction address (for the run report).
struct EmulationHit {
  uint64_t address;
  uint64_t count;
};
std::vector<EmulationHit> emulation_hits();

}  // namespace am::rt
