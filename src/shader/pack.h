#pragma once

// The game's shader pack: an RPF9 archive whose table of contents is encrypted, but whose shader
// records are framed in the clear. Each record is
//
//   u8 n | name (n - 1 bytes) | 0 | u8 flags | u32 packed size | u32 ? | u32 packed size | u32 size
//   | raw DEFLATE stream (packed size bytes) -> a DXBC shader container of `size` bytes
//
// so the records can be found by scanning, without the table of contents. A candidate counts only
// if it inflates to exactly `size` bytes starting with "DXBC".

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace am::shader {

struct PackRecord {
  std::string name;
  size_t offset = 0;  // of the length byte
  uint8_t flags = 0;
  uint32_t unknown = 0;
  size_t data = 0;  // offset of the DEFLATE stream
  uint32_t packed_size = 0;
  uint32_t size = 0;
};

struct PackScanStats {
  size_t records = 0;
  size_t rejected = 0;  // framed like a record but did not inflate to a container
};

// Calls `on_record` with each record and its inflated container, in file order.
void scan_pack(std::span<const uint8_t> pack,
               const std::function<void(const PackRecord&, std::span<const uint8_t> container)>& on_record,
               PackScanStats& stats);

}  // namespace am::shader
