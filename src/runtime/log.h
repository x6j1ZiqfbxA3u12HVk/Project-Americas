#pragma once

// Run log: every line goes to stderr and, once opened, to <root>\logs\run.log.

#include <sal.h>

#include <filesystem>
#include <string>

namespace am::rt {

void log_open(const std::filesystem::path& file);
void log_line(const std::string& line);
void logf(_Printf_format_string_ const char* format, ...);

}  // namespace am::rt
