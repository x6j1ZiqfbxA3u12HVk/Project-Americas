#include "runtime/console.h"

namespace am::rt {
namespace {

constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kGiB = uint64_t{1} << 30;

// Title memory as the consoles give it to a game: Series X 13.5 GiB (10 GiB of it the faster
// GPU-optimal memory), Series S about 7.6 GiB in one speed class.
constexpr ConsoleProfile kSeriesS{Console::SeriesS, "series_s", 7776 * kMiB, 0};
constexpr ConsoleProfile kSeriesX{Console::SeriesX, "series_x", 13 * kGiB + 512 * kMiB, 10 * kGiB};

// Series X wants its 13.5 GiB on top of Windows and our own layer.
constexpr uint64_t kSeriesXHostMemory = 24 * kGiB;

Console g_console = Console::SeriesS;

}  // namespace

const ConsoleProfile& profile_of(Console console) { return console == Console::SeriesX ? kSeriesX : kSeriesS; }

bool choose_console(std::string_view setting, uint64_t host_physical_bytes, Console* out) {
  if (setting.empty() || setting == "auto") {
    // A machine sold as "24 GB" reports a little less to Windows.
    *out = host_physical_bytes + kGiB >= kSeriesXHostMemory ? Console::SeriesX : Console::SeriesS;
    return true;
  }
  if (setting == "series_s") {
    *out = Console::SeriesS;
    return true;
  }
  if (setting == "series_x") {
    *out = Console::SeriesX;
    return true;
  }
  return false;
}

void set_console(Console console) { g_console = console; }
const ConsoleProfile& current_console() { return profile_of(g_console); }

}  // namespace am::rt
