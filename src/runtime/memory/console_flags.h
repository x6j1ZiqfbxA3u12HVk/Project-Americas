#pragma once

// The console's additions to the memory API, and how each becomes a host request.
//
// The console's VirtualAlloc and XMemVirtualAlloc take the host's MEM_* and PAGE_* constants plus
// console bits; XMemVirtualAlloc also takes 64 bits of XMem flags. Everything here was read from
// the game's own call sites (see docs/PLAN.md, section 11, "M2.1"):
//
// * allocation type: bit 29 selects 2 MiB pages; bits 29 and 22 together select 64 KiB pages.
//   On the host those bits mean MEM_LARGE_PAGES and MEM_PHYSICAL, so they never pass through.
// * protection: the low bits are the host's (PAGE_NOACCESS ... PAGE_GUARD). PAGE_NOCACHE and
//   PAGE_WRITECOMBINE describe the console's CPU caching of GPU-shared memory; our layer reads
//   that memory on the CPU (uploads), where uncached reads would be very slow, so they are
//   dropped. Bits 0x800-0x40000 are the GPU's access (the SDK names them PAGE_GRAPHICS_*); they
//   are kept for the graphics layer and removed from the host request.
// * XMem flags: the low two bits are 1 for CPU memory and 3 for memory the GPU maps too; bit 2
//   asks for an address below 4 GiB. The other bits seen (5, 7-9) are recorded, not acted on.
//
// A reservation of 64 KiB pages without a commit is where the title later maps physical pages
// (XMemMapPhysicalPages). On the host only a placeholder can take a mapped view, so such a
// reservation is kept as a placeholder.

#include <cstdint>

namespace am::rt::mem {

constexpr uint32_t kConsolePages2M = 0x20000000;   // allocation type: 2 MiB pages
constexpr uint32_t kConsolePages64K = 0x00400000;  // with kConsolePages2M: 64 KiB pages
constexpr uint32_t kGpuAccessMask = 0x0007F800;    // protection: the GPU's access bits
constexpr uint64_t kXMemGraphics = 0x2;            // XMem flags: the GPU maps it too
constexpr uint64_t kXMemCpu = 0x1;                 // XMem flags: CPU memory
constexpr uint64_t kXMem32Bit = 0x4;               // XMem flags: below 4 GiB
constexpr uint64_t kXMemKnown = 0x3A7;             // bits seen in the game's calls

// Title memory lies where the console keeps it: above 4 GiB (which stays free for requests that
// need a 32-bit address) and below 2^40, the GPU's address width. The ray-tracing BVH builder
// refuses addresses above that.
constexpr uint64_t kTitleLowest = uint64_t{1} << 32;
constexpr uint64_t kTitleHighest = (uint64_t{1} << 40) - 1;
constexpr uint64_t kLow32Lowest = 0x10000;
constexpr uint64_t kLow32Highest = 0xFFFFFFFF;

constexpr uint64_t kPage4K = 0x1000;
constexpr uint64_t kPage64K = 0x10000;
constexpr uint64_t kPage2M = 0x200000;

struct ConsoleRequest {
  uint64_t address = 0;     // 0: anywhere
  uint64_t size = 0;
  uint32_t type = 0;        // MEM_* plus console page-size bits
  uint64_t xmem_flags = 0;  // XMemVirtualAlloc's flags; 0 from VirtualAlloc
  uint32_t protect = 0;     // PAGE_* plus console GPU access bits
  uint64_t alignment = 0;   // ours (XMemAllocDefault): a larger alignment than the page size
};

enum class Action : uint8_t {
  Host,             // call VirtualAlloc2 with the fields below
  KeepPlaceholder,  // the title turns its own placeholder into a physical-page reservation:
                    // on the host it stays the placeholder it is; no host call
  Refuse,           // invalid on the console too
};

struct HostRequest {
  Action action = Action::Host;
  uint64_t page_size = kPage4K;
  uint64_t size = 0;            // rounded up to whole pages
  uint32_t type = 0;            // host MEM_* flags
  uint32_t protect = 0;         // host PAGE_* flags
  uint64_t alignment = 0;       // 0: the host's (64 KiB)
  uint64_t lowest = 0, highest = 0;  // where a new allocation may lie; both 0 at a given address
  bool physical_reservation = false;  // pages get mapped into it later: a host placeholder
  uint32_t gpu_access = 0;            // console GPU access bits, for the graphics layer
  uint32_t cpu_protect = 0;           // the CPU protection mapped physical pages get
  uint32_t dropped_protect = 0;       // console caching bits not passed to the host
  uint64_t unknown_flags = 0;         // XMem flag bits we have not seen the game use
};

HostRequest translate(const ConsoleRequest& request);

// A protection for VirtualProtect: the host part of a console protection.
uint32_t host_protection(uint32_t console_protect);

}  // namespace am::rt::mem
