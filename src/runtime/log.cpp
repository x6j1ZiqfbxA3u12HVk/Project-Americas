#include "runtime/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace am::rt {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_file = INVALID_HANDLE_VALUE;

}  // namespace

void log_open(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  AcquireSRWLockExclusive(&g_lock);
  if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
  g_file = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
  ReleaseSRWLockExclusive(&g_lock);
}

void log_line(const std::string& line) {
  const std::string text = line + "\n";
  AcquireSRWLockExclusive(&g_lock);
  std::fwrite(text.data(), 1, text.size(), stderr);
  std::fflush(stderr);
  if (g_file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(g_file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  }
  ReleaseSRWLockExclusive(&g_lock);
}

void logf(const char* format, ...) {
  char buf[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  log_line(buf);
}

}  // namespace am::rt
