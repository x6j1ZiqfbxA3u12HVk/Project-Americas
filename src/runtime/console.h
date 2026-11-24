#pragma once

// Which console the title believes it runs on. The choice sizes the title's memory budget (and,
// once XGameRuntime exists, what XSystem reports). Plan, section 10, decision 2: chosen from the
// host's memory unless americas.ini or --console says otherwise.

#include <cstdint>
#include <string_view>

namespace am::rt {

enum class Console : uint8_t { SeriesS, SeriesX };

struct ConsoleProfile {
  Console console = Console::SeriesS;
  const char* name = "";       // "series_s"
  uint64_t title_memory = 0;   // bytes the title may use, physical pages included
  uint64_t gpu_optimal = 0;    // of that, the faster GPU-optimal memory (Series X only)
};

const ConsoleProfile& profile_of(Console console);

// `setting` is auto, series_s or series_x. Auto picks Series X when the host has the memory to
// back its larger title memory. False for an unknown setting.
bool choose_console(std::string_view setting, uint64_t host_physical_bytes, Console* out);

// The process's choice, made before the title starts.
void set_console(Console console);
const ConsoleProfile& current_console();

}  // namespace am::rt
