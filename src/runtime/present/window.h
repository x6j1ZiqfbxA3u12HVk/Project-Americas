#pragma once

// The game's window: made on first use, on a thread of its own that runs its messages, so the
// title's threads never have to. Closing it ends the run (the report is written first).

#include <windows.h>

#include <string>

namespace am::rt::present {

// The window, made the first time it is asked for; nullptr if it could not be made.
HWND game_window();
// Replaces the text after the window's name in its title bar.
void set_window_status(const std::wstring& status);

}  // namespace am::rt::present
