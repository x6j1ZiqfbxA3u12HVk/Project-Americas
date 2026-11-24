#include "runtime/memory/default_alloc.h"

#include <windows.h>

#include <algorithm>
#include <bit>

namespace am::rt::mem {
namespace {

constexpr uint64_t kSmallest = 32;
constexpr uint64_t kLargestSmall = kSmallest << 10;  // 32 KiB
constexpr uint64_t kSlab = 64 * 1024;                // what a size class takes from an arena at once
constexpr uint64_t kArena = 2 * 1024 * 1024;
constexpr uint32_t kMagic = 0x41584d41;              // "AMXA"
constexpr uint64_t kAllFlags = kXMemCpu | kXMemGraphics;

// Just below each small block's user pointer.
struct Header {
  uint32_t magic;
  uint16_t size_class;
  uint16_t offset;  // from the block's start
};
static_assert(sizeof(Header) == 8);

}  // namespace

uint64_t xalloc_alignment(uint64_t attributes) {
  const uint64_t shift = std::max<uint64_t>((attributes >> 24) & 0x1F, 4);
  return uint64_t{1} << shift;
}

bool DefaultAllocator::refill(int size_class) {
  if (arena_left_ < kSlab) {
    ConsoleRequest r;
    r.size = kArena;
    r.type = MEM_RESERVE | MEM_COMMIT | kConsolePages2M;
    r.xmem_flags = kAllFlags;
    r.protect = PAGE_READWRITE;
    arena_ = static_cast<uint8_t*>(memory_.allocate(r));
    if (!arena_) return false;
    arena_left_ = kArena;
  }
  const uint64_t block = kSmallest << size_class;
  for (uint64_t at = 0; at + block <= kSlab; at += block) {
    auto* b = reinterpret_cast<FreeBlock*>(arena_ + at);
    b->next = free_[size_class];
    free_[size_class] = b;
  }
  arena_ += kSlab;
  arena_left_ -= kSlab;
  return true;
}

void* DefaultAllocator::allocate(uint64_t size, uint64_t attributes) {
  const uint64_t alignment = xalloc_alignment(attributes);
  if (size > (uint64_t{1} << 40)) return nullptr;
  // A block starts on a multiple of its size, so a class at least the alignment keeps it.
  const uint64_t front = std::max<uint64_t>(alignment, 16);
  if (size + front > kLargestSmall) {
    ConsoleRequest r;
    r.size = std::max<uint64_t>(size, 1);
    r.type = MEM_RESERVE | MEM_COMMIT;
    r.xmem_flags = kAllFlags;
    r.protect = PAGE_READWRITE;
    r.alignment = alignment;
    return memory_.allocate(r);
  }
  const uint64_t need = std::bit_ceil(std::max<uint64_t>(size + front, kSmallest));
  const int size_class = std::countr_zero(need) - std::countr_zero(kSmallest);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!free_[size_class] && !refill(size_class)) return nullptr;
  FreeBlock* b = free_[size_class];
  free_[size_class] = b->next;
  auto* start = reinterpret_cast<uint8_t*>(b);
  uint8_t* user = start + front;
  *reinterpret_cast<Header*>(user - sizeof(Header)) =
      Header{kMagic, static_cast<uint16_t>(size_class), static_cast<uint16_t>(front)};
  return user;
}

void DefaultAllocator::free(void* address, uint64_t /*attributes*/) {
  if (!address) return;
  if (memory_.is_allocation(address)) {
    memory_.free(address, 0, MEM_RELEASE);
    return;
  }
  auto* user = static_cast<uint8_t*>(address);
  Header& h = *reinterpret_cast<Header*>(user - sizeof(Header));
  if (h.magic != kMagic || h.size_class >= kClasses) return;  // not ours: ignored, as a free of garbage
  h.magic = 0;
  auto* b = reinterpret_cast<FreeBlock*>(user - h.offset);
  std::lock_guard<std::mutex> lock(mutex_);
  b->next = free_[h.size_class];
  free_[h.size_class] = b;
}

DefaultAllocator& default_allocator() {
  static DefaultAllocator* const allocator = new DefaultAllocator(title_memory());
  return *allocator;
}

}  // namespace am::rt::mem
