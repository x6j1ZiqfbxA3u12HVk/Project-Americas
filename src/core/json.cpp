#include "core/json.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace am {
namespace {

bool valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

}  // namespace

void JsonWriter::escape_into(std::string& out, std::string_view s) {
  const bool utf8 = valid_utf8(s);
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; continue;
      case '\\': out += "\\\\"; continue;
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      default: break;
    }
    if (c < 0x20 || (c >= 0x80 && !utf8)) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
      out += buf;
    } else {
      out += ch;
    }
  }
}

void JsonWriter::newline() {
  out_ += '\n';
  out_.append(stack_.size() * 2, ' ');
}

void JsonWriter::before_value() {
  if (stack_.empty()) return;
  Level& top = stack_.back();
  if (top.object) return;  // key() already placed the separator
  if (!top.empty) out_ += ',';
  top.empty = false;
  newline();
}

JsonWriter& JsonWriter::key(std::string_view k) {
  Level& top = stack_.back();
  if (!top.empty) out_ += ',';
  top.empty = false;
  newline();
  out_ += '"';
  escape_into(out_, k);
  out_ += "\": ";
  return *this;
}

JsonWriter& JsonWriter::begin_object() {
  before_value();
  out_ += '{';
  stack_.push_back({true, true});
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  const bool was_empty = stack_.back().empty;
  stack_.pop_back();
  if (!was_empty) newline();
  out_ += '}';
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  before_value();
  out_ += '[';
  stack_.push_back({false, true});
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  const bool was_empty = stack_.back().empty;
  stack_.pop_back();
  if (!was_empty) newline();
  out_ += ']';
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view s) {
  before_value();
  out_ += '"';
  escape_into(out_, s);
  out_ += '"';
  return *this;
}

JsonWriter& JsonWriter::value(bool b) {
  before_value();
  out_ += b ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::value(double d) {
  if (!std::isfinite(d)) return null();
  before_value();
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, d);
  out_.append(buf, r.ptr);
  return *this;
}

JsonWriter& JsonWriter::null() {
  before_value();
  out_ += "null";
  return *this;
}

}  // namespace am
