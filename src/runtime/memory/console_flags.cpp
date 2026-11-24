#include "runtime/memory/console_flags.h"

#include <windows.h>

namespace am::rt::mem {
namespace {

constexpr uint32_t kHostProtectMask = 0xFF | PAGE_GUARD;
constexpr uint32_t kCachingBits = PAGE_NOCACHE | PAGE_WRITECOMBINE;

uint64_t round_up(uint64_t value, uint64_t to) { return (value + to - 1) & ~(to - 1); }

}  // namespace

uint32_t host_protection(uint32_t console_protect) {
  const uint32_t p = console_protect & ~(kGpuAccessMask | kCachingBits);
  // Only GPU access: the CPU may not touch it.
  return (p & kHostProtectMask) == 0 ? (PAGE_NOACCESS | (p & ~kHostProtectMask)) : p;
}

HostRequest translate(const ConsoleRequest& in) {
  HostRequest out;
  const bool pages64k = (in.type & (kConsolePages2M | kConsolePages64K)) == (kConsolePages2M | kConsolePages64K);
  const bool pages2m = !pages64k && (in.type & kConsolePages2M);
  out.page_size = pages64k ? kPage64K : pages2m ? kPage2M : kPage4K;
  if (in.size == 0 || in.size > ~uint64_t{0} - out.page_size) {
    out.action = Action::Refuse;
    return out;
  }
  if (in.alignment & (in.alignment - 1)) {
    out.action = Action::Refuse;
    return out;
  }
  // A new allocation is made of whole pages; at a given address the title's size stands.
  out.size = in.address ? in.size : round_up(in.size, out.page_size);
  out.gpu_access = in.protect & kGpuAccessMask;
  out.dropped_protect = in.protect & kCachingBits;
  out.protect = host_protection(in.protect);
  out.cpu_protect = out.protect;
  out.unknown_flags = in.xmem_flags & ~kXMemKnown;

  uint32_t type = in.type & ~(kConsolePages2M | kConsolePages64K);
  out.physical_reservation = pages64k && !(type & MEM_COMMIT);
  if (out.physical_reservation) {
    if (type & MEM_REPLACE_PLACEHOLDER) {
      // A piece the title split off its own placeholder becomes a physical-page reservation.
      if (!in.address) out.action = Action::Refuse;
      else out.action = Action::KeepPlaceholder;
      return out;
    }
    type = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    out.protect = PAGE_NOACCESS;  // placeholders take no other protection
  }
  if (!in.address) {
    type |= MEM_RESERVE;  // the console's MEM_COMMIT alone reserves too
    if (in.xmem_flags & kXMem32Bit) {
      out.lowest = kLow32Lowest;
      out.highest = kLow32Highest;
    } else {
      out.lowest = kTitleLowest;
      out.highest = kTitleHighest;
    }
    if (out.page_size == kPage2M) out.alignment = kPage2M;
    if (in.alignment > kPage64K && in.alignment > out.alignment) out.alignment = in.alignment;
  }
  out.type = type;
  return out;
}

}  // namespace am::rt::mem
