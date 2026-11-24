#include "core/text.h"

#include <windows.h>

#include <cstdio>

namespace am {

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int in = static_cast<int>(utf8.size());
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), in, nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), in, out.data(), n);
  return out;
}

std::string narrow(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int in = static_cast<int>(wide.size());
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), in, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), in, out.data(), n, nullptr, nullptr);
  return out;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string to_lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = ascii_lower(c);
  return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

bool istarts_with_ascii(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals_ascii(s.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string_view::npos) return {};
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

std::string format_bytes(uint64_t bytes) {
  static const char* const kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int unit = 0;
  while (v >= 1024.0 && unit < 4) {
    v /= 1024.0;
    ++unit;
  }
  char buf[32];
  if (unit == 0) {
    std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buf, sizeof buf, "%.1f %s", v, kUnits[unit]);
  }
  return buf;
}

std::string hex(uint64_t value, int min_digits) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%0*llx", min_digits, static_cast<unsigned long long>(value));
  return buf;
}

std::string hex_bytes(const void* data, size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out += kDigits[p[i] >> 4];
    out += kDigits[p[i] & 15];
  }
  return out;
}

}  // namespace am
