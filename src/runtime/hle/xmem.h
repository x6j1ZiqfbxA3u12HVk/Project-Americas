#pragma once

// XMemAlloc and XMemFree for the runtime's own console libraries: the console's graphics
// runtime takes memory through them, so the title's allocation hooks see it as on the console.

#include <cstddef>
#include <cstdint>

namespace am::rt::hle {

void* xmem_alloc(size_t size, uint64_t attributes);
void xmem_free(void* address, uint64_t attributes);

// XALLOC attribute fields (see memory/default_alloc.h for the layout).
namespace xalloc {
constexpr uint64_t kPage4K = 0, kPage64K = 1, kPageLarge = 2;  // large: 2 MiB
constexpr uint64_t kAllocatorD3D = 128;  // the first id reserved for the system's libraries

constexpr uint64_t attributes(uint64_t page_size, uint64_t allocator, uint32_t alignment_shift) {
  return (page_size << 14) | (allocator << 16) | (uint64_t{alignment_shift & 0x1F} << 24);
}
constexpr uint64_t page_size(uint64_t attributes) { return (attributes >> 14) & 3; }
constexpr uint64_t allocator(uint64_t attributes) { return (attributes >> 16) & 0xFF; }
}  // namespace xalloc

}  // namespace am::rt::hle
