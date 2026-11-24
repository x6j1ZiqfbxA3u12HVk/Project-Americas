#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <vector>

namespace am {

// A small streaming JSON writer with two-space indentation. Strings that are not valid UTF-8
// (names read from binaries) have their high bytes escaped as \u00XX, so the output stays valid.
class JsonWriter {
 public:
  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(std::string_view k);

  JsonWriter& value(std::string_view s);
  JsonWriter& value(const char* s) { return value(std::string_view(s ? s : "")); }
  JsonWriter& value(const std::string& s) { return value(std::string_view(s)); }
  JsonWriter& value(bool b);
  JsonWriter& value(double d);
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  JsonWriter& value(T v) {
    before_value();
    out_ += std::to_string(v);
    return *this;
  }
  JsonWriter& null();

  template <class T>
  JsonWriter& field(std::string_view k, const T& v) {
    key(k);
    return value(v);
  }

  const std::string& str() const { return out_; }

  static void escape_into(std::string& out, std::string_view s);

 private:
  struct Level {
    bool object;
    bool empty;
  };
  void before_value();
  void newline();

  std::string out_;
  std::vector<Level> stack_;
};

}  // namespace am
