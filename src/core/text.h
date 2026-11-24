#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace am {

// UTF-8 <-> UTF-16 for Windows APIs. Invalid input becomes U+FFFD.
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

char ascii_lower(char c);
std::string to_lower_ascii(std::string_view s);
bool iequals_ascii(std::string_view a, std::string_view b);
bool istarts_with_ascii(std::string_view s, std::string_view prefix);
std::string_view trim(std::string_view s);

// "17 B", "512.0 KiB", "15.9 GiB"
std::string format_bytes(uint64_t bytes);
// Lower-case hex without "0x", padded to at least `min_digits`.
std::string hex(uint64_t value, int min_digits = 0);
std::string hex_bytes(const void* data, size_t size);

}  // namespace am
