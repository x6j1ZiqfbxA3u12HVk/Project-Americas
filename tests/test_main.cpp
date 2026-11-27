#include <windows.h>
// windows.h first
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

#include "test.h"

#pragma comment(lib, "dbghelp.lib")

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

namespace {

const char* g_current = "(none)";

// A crash in a test: the exception, the test and the stack with symbols, then the usual end.
LONG WINAPI report_crash(EXCEPTION_POINTERS* e) {
  std::printf("CRASH  %s: exception 0x%08lX at %p\n", g_current, e->ExceptionRecord->ExceptionCode,
              e->ExceptionRecord->ExceptionAddress);
  HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  CONTEXT ctx = *e->ContextRecord;
  STACKFRAME64 f = {};
  f.AddrPC.Offset = ctx.Rip;
  f.AddrFrame.Offset = ctx.Rbp;
  f.AddrStack.Offset = ctx.Rsp;
  f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
  for (int depth = 0; depth < 40; ++depth) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &f, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                     nullptr) ||
        f.AddrPC.Offset == 0)
      break;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 off = 0;
    DWORD line_off = 0;
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof line;
    const bool named = SymFromAddr(process, f.AddrPC.Offset, &off, sym) != FALSE;
    const bool lined = SymGetLineFromAddr64(process, f.AddrPC.Offset, &line_off, &line) != FALSE;
    std::printf("  %2d %s+0x%llx", depth, named ? sym->Name : "?", static_cast<unsigned long long>(off));
    if (lined) std::printf(" (%s:%lu)", line.FileName, line.LineNumber);
    std::printf("\n");
  }
  std::fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// am_tests [name-fragment]: runs every test, or those whose name contains the fragment.
int main(int argc, char** argv) {
  SetUnhandledExceptionFilter(report_crash);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : am::test::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = am::test::g_failures;
    g_current = c.name;
    c.fn();
    std::fflush(stdout);
    ++run;
    const bool ok = am::test::g_failures == before;
    if (!ok) ++failed;
    std::printf("%s %s\n", ok ? "ok    " : "FAIL  ", c.name);
  }
  std::printf("%d test(s), %d failed\n", run, failed);
  return failed || run == 0 ? 1 : 0;
}
