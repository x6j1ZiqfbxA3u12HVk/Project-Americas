#pragma once

// A minimal test runner: TEST_CASE registers a function, CHECK records a failure and goes on.

#include <vector>

namespace am::test {

struct Case {
  const char* name;
  void (*fn)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const char* expr);

struct Register {
  Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace am::test

#define AM_TEST_CONCAT2(a, b) a##b
#define AM_TEST_CONCAT(a, b) AM_TEST_CONCAT2(a, b)
#define TEST_CASE(name)                                                         \
  static void name();                                                           \
  static ::am::test::Register AM_TEST_CONCAT(register_, name)(#name, &name);    \
  static void name()
#define CHECK(expr)                                         \
  do {                                                      \
    if (!(expr)) ::am::test::fail(__FILE__, __LINE__, #expr); \
  } while (0)
