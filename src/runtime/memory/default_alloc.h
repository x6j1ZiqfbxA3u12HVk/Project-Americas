#pragma once

// XMemAllocDefault / XMemFreeDefault: the system allocator behind XMemAlloc, for allocators the
// title has not registered one of its own for.
//
// The attributes are 64 bits: object type (bits 0-13), page size (14-15), allocator id (16-23,
// the field the game's own hook dispatches on), alignment as a power of two (24-28), memory type
// (29 and up). Blocks live in title memory, below 2^40 like the console's, because the GPU and
// the ray-tracing BVH builder may use them: large blocks get an allocation of their own, small
// ones come from size classes carved out of 2 MiB arenas.

#include <cstdint>
#include <mutex>
#include <vector>

#include "runtime/memory/title_memory.h"

namespace am::rt::mem {

uint64_t xalloc_alignment(uint64_t attributes);  // at least 16 bytes

class DefaultAllocator {
 public:
  explicit DefaultAllocator(TitleMemory& memory) : memory_(memory) {}
  DefaultAllocator(const DefaultAllocator&) = delete;
  DefaultAllocator& operator=(const DefaultAllocator&) = delete;

  void* allocate(uint64_t size, uint64_t attributes);
  void free(void* address, uint64_t attributes);

 private:
  static constexpr int kClasses = 11;  // 32 B ... 32 KiB
  struct FreeBlock {
    FreeBlock* next;
  };
  bool refill(int size_class);

  TitleMemory& memory_;
  std::mutex mutex_;
  FreeBlock* free_[kClasses] = {};
  uint8_t* arena_ = nullptr;  // the current arena's unused part
  uint64_t arena_left_ = 0;
};

DefaultAllocator& default_allocator();  // over title_memory()

}  // namespace am::rt::mem
