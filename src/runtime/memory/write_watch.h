#pragma once

// Writes to title memory, seen as they happen. A watched range is read-only until the title's
// first write to it: the fault handler makes it writable again, lets the write through and
// reports the range as written (once; it is then no longer watched). The GPU's copies of the
// title's textures use it: a texture is uploaded again before the next submission that
// follows a write, with nothing hashed.
//
// Only whole 4 KiB pages of committed private read-write memory are watched: a view of
// physical pages is not, since the title may write those pages through another view. Writes by the kernel (ReadFile into a watched buffer) do not fault but fail:
// code that has the kernel write title memory releases the range first
// (release_for_host_write).

#include <cstdint>
#include <utility>
#include <vector>

namespace am::rt::mem {

// Watches [address, address + size) (rounded out to whole pages); false when it cannot, and
// then nothing is watched. Overlapping watches merge.
bool watch_writes(uint64_t address, uint64_t size);
// The ranges written since the last call, as [first, end) pairs; they are no longer watched.
void take_written(std::vector<std::pair<uint64_t, uint64_t>>& out);
// The kernel is about to write [address, address + size): watched ranges over it are made
// writable and reported written.
void release_for_host_write(const void* address, uint64_t size);

}  // namespace am::rt::mem
