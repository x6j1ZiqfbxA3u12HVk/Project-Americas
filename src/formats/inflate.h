#pragma once

// DEFLATE decompression (RFC 1951), raw or zlib-wrapped (RFC 1950). Used for the game's packed
// records and, later, for the console's DirectStorage zlib streams.

#include <cstddef>
#include <cstdint>
#include <span>

namespace am::formats {

enum class InflateStatus : uint8_t {
  Ok,
  Corrupt,     // not a valid stream
  Truncated,   // the input ended before the final block did
  OutputFull,  // the output buffer is too small
};

const char* inflate_status_name(InflateStatus s);

struct InflateResult {
  InflateStatus status = InflateStatus::Corrupt;
  size_t written = 0;   // bytes produced
  size_t consumed = 0;  // input bytes used (the final block's last byte included)
};

// A raw DEFLATE stream (no zlib or gzip header).
InflateResult inflate_raw(std::span<const uint8_t> in, std::span<uint8_t> out);

// A zlib stream: header, DEFLATE data, Adler-32 (checked).
InflateResult inflate_zlib(std::span<const uint8_t> in, std::span<uint8_t> out);

uint32_t adler32(std::span<const uint8_t> data, uint32_t start = 1);

}  // namespace am::formats
