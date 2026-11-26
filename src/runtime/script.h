#pragma once

// AMERICAS_SCRIPT: driving the menus by what the game does, not by the clock. Steps, separated
// by ';' (or one a line in a file named "@path"):
//
//   A | B | X | Y | MENU | VIEW | UP | DOWN | LEFT | RIGHT   tap the stand-in pad's button
//   A x4                    tap it four times, a second apart
//   wait 2.5                wait that many seconds
//   log <text>              wait for a log line holding <text> (from this step on)
//   draw <text>             wait for a draw whose pipeline's description holds <text> (a
//                           program id, as the frame log prints them)
//   A until draw <text>     tap A every 1.5 s until such a draw (or: until log <text>)
//   capture                 save the next shown frame as <logs>\script-<n>.bmp
//   trace                   from now on, log each pipeline the first time it draws (to find
//                           a page's own pipelines for `draw` steps)
//   quit                    end the run
//
// Each step is logged as it starts ("script: ..."). The script runs on a thread of its own.

#include <cstdint>
#include <functional>
#include <string>

namespace am::rt {

// Starts the script, when AMERICAS_SCRIPT names one (once).
void script_start();
// The stand-in pad buttons the script holds down now.
uint32_t script_buttons();
// Every log line passes here (cheap unless a step waits for one).
void script_on_log(const std::string& line);
// A draw with pipeline `pipeline`: `describe` gives its description, asked once per pipeline
// and only while a step waits for draws.
void script_on_draw(const void* pipeline, const std::function<std::string()>& describe);
// Whether the script wants the frame about to be shown saved (and as what, for the file name).
bool script_wants_capture(uint32_t& number);

}  // namespace am::rt
