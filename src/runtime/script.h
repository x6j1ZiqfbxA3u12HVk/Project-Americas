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
//   A next                  tap A, wait until the page changes (the set of draws a frame makes,
//                           by pipeline and vertex count, is not the one before the tap) and
//                           then settles (that set unchanged, and no file or DirectStorage
//                           reads, for 0.35 s); at most
//                           2 s for the change and 8 s in all ("A next 30": 30 s in all, for a
//                           loading screen). "A x4 next" settles after each tap.
//   settle [seconds]        wait for the page to settle, as after a tap with `next`
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
// `shape` tells draws of one pipeline apart (their vertex and instance counts).
void script_on_draw(const void* pipeline, const std::function<std::string()>& describe, uint64_t shape = 0);
// Whether the script wants the frame about to be shown saved (and as what, for the file name).
bool script_wants_capture(uint32_t& number);
// A frame was shown (after its draws).
void script_on_frame();
// The game read from storage (a file read or a DirectStorage request): it is loading.
void script_on_load();

}  // namespace am::rt
