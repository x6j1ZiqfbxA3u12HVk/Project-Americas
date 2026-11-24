#include <cstdio>
#include <cstring>

#include "test.h"

namespace am::test {

static int g_failures = 0;

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

void fail(const char* file, int line, const char* expr) {
  ++g_failures;
  std::printf("  FAILED %s(%d): %s\n", file, line, expr);
}

}  // namespace am::test

// am_tests [name-fragment]: runs every test, or those whose name contains the fragment.
int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : am::test::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = am::test::g_failures;
    c.fn();
    ++run;
    const bool ok = am::test::g_failures == before;
    if (!ok) ++failed;
    std::printf("%s %s\n", ok ? "ok    " : "FAIL  ", c.name);
  }
  std::printf("%d test(s), %d failed\n", run, failed);
  return failed || run == 0 ? 1 : 0;
}
