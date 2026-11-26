#include "runtime/script.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime/log.h"

namespace am::rt {
namespace {

// The stand-in pad's buttons, as GameInput numbers them.
struct ButtonName {
  const char* name;
  uint32_t bit;
};
constexpr ButtonName kButtons[] = {{"MENU", 0x1}, {"VIEW", 0x2},  {"A", 0x4},    {"B", 0x8},     {"X", 0x10},
                                   {"Y", 0x20},   {"UP", 0x40},   {"DOWN", 0x80}, {"LEFT", 0x100}, {"RIGHT", 0x200}};

uint32_t button_of(const std::string& word) {
  for (const ButtonName& b : kButtons) {
    if (!_stricmp(b.name, word.c_str())) return b.bit;
  }
  return 0;
}

struct Step {
  enum Kind { Tap, Wait, Log, Draw, Capture, Trace, Quit } kind = Wait;
  uint32_t button = 0;  // Tap
  int count = 1;        // Tap: how many times; with `until`, until the condition holds
  bool until = false;   // Tap until a Log or Draw condition
  bool until_draw = false;
  double seconds = 0;  // Wait
  std::string text;    // Log, Draw, and the condition of `until`
  std::string source;  // as written, for the log
};

std::string trim(std::string s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  const size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool parse(const std::string& line, Step& step) {
  std::istringstream in(line);
  std::string word;
  in >> word;
  step.source = line;
  std::string rest;
  std::getline(in, rest);
  rest = trim(rest);
  if (!_stricmp(word.c_str(), "wait")) {
    step.kind = Step::Wait;
    step.seconds = std::atof(rest.c_str());
    return true;
  }
  if (!_stricmp(word.c_str(), "log") || !_stricmp(word.c_str(), "draw")) {
    step.kind = !_stricmp(word.c_str(), "log") ? Step::Log : Step::Draw;
    step.text = rest;
    return !rest.empty();
  }
  if (!_stricmp(word.c_str(), "capture")) {
    step.kind = Step::Capture;
    return true;
  }
  if (!_stricmp(word.c_str(), "trace")) {
    step.kind = Step::Trace;
    return true;
  }
  if (!_stricmp(word.c_str(), "quit")) {
    step.kind = Step::Quit;
    return true;
  }
  step.kind = Step::Tap;
  step.button = button_of(word);
  if (!step.button) return false;
  if (rest.size() > 1 && (rest[0] == 'x' || rest[0] == 'X') && std::isdigit(static_cast<unsigned char>(rest[1]))) {
    step.count = std::atoi(rest.c_str() + 1);
    return step.count > 0;
  }
  if (rest.rfind("until ", 0) == 0) {
    std::istringstream cond(rest.substr(6));
    std::string what;
    cond >> what;
    std::string text;
    std::getline(cond, text);
    step.until = true;
    step.until_draw = !_stricmp(what.c_str(), "draw");
    step.text = trim(text);
    return (step.until_draw || !_stricmp(what.c_str(), "log")) && !step.text.empty();
  }
  return rest.empty();
}

std::vector<Step> load() {
  std::vector<Step> steps;
  char text[4096] = {};
  if (!GetEnvironmentVariableA("AMERICAS_SCRIPT", text, sizeof text)) return steps;
  std::string all = text;
  if (!all.empty() && all[0] == '@') {
    std::ifstream f(all.substr(1));
    std::stringstream ss;
    ss << f.rdbuf();
    all = ss.str();
  }
  std::string item;
  for (char c : all + ";") {
    if (c == ';' || c == '\n') {
      item = trim(item);
      if (!item.empty() && item[0] != '#') {
        Step s;
        if (parse(item, s)) {
          steps.push_back(s);
        } else {
          logf("script: cannot read the step \"%s\"", item.c_str());
        }
      }
      item.clear();
    } else {
      item += c;
    }
  }
  return steps;
}

struct Script {
  std::atomic<uint32_t> buttons{0};
  // The condition a step waits for (empty: none), and whether it was met.
  std::mutex lock;
  std::string wanted;
  bool wanted_draw = false;
  std::atomic<bool> waiting{false}, waiting_draws{false}, met{false}, tracing{false};
  std::unordered_map<const void*, std::string> described;  // pipeline -> description
  std::atomic<int> capture{0};                              // frames to save: 1 asked
  std::atomic<uint32_t> captures{0};

  void expect(const std::string& text, bool draw) {
    std::lock_guard g(lock);
    wanted = text;
    wanted_draw = draw;
    met = false;
    waiting_draws = draw;
    waiting = true;
  }
  void done_waiting() {
    waiting = false;
    waiting_draws = false;
  }
  void note(const std::string& text, bool draw) {
    std::lock_guard g(lock);
    if (waiting && wanted_draw == draw && text.find(wanted) != std::string::npos) met = true;
  }

  void tap(uint32_t button) {
    buttons |= button;
    Sleep(150);
    buttons &= ~button;
    Sleep(150);
  }

  void run(std::vector<Step> steps) {
    for (size_t i = 0; i < steps.size(); ++i) {
      const Step& s = steps[i];
      logf("script: step %zu: %s", i + 1, s.source.c_str());
      switch (s.kind) {
        case Step::Wait: Sleep(static_cast<DWORD>(s.seconds * 1000)); break;
        case Step::Log:
        case Step::Draw:
          expect(s.text, s.kind == Step::Draw);
          while (!met) Sleep(20);
          done_waiting();
          break;
        case Step::Tap:
          if (!s.until) {
            for (int k = 0; k < s.count; ++k) {
              tap(s.button);
              if (k + 1 < s.count) Sleep(700);
            }
          } else {
            expect(s.text, s.until_draw);
            int taps = 0;
            while (!met) {
              tap(s.button);
              ++taps;
              for (int w = 0; w < 60 && !met; ++w) Sleep(20);  // 1.5 s a tap, as the condition allows
            }
            done_waiting();
            logf("script: step %zu met after %d taps", i + 1, taps);
          }
          break;
        case Step::Capture:
          capture = 1;
          while (capture) Sleep(10);
          break;
        case Step::Trace: tracing = true; break;
        case Step::Quit:
          logf("script: quit");
          Sleep(200);
          TerminateProcess(GetCurrentProcess(), 0);
          break;
      }
    }
    logf("script: done");
  }
};

Script& script() {
  static Script* const s = new Script;
  return *s;
}

}  // namespace

void script_start() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::vector<Step> steps = load();
    if (steps.empty()) return;
    logf("script: %zu steps (AMERICAS_SCRIPT)", steps.size());
    std::thread([steps = std::move(steps)]() mutable { script().run(std::move(steps)); }).detach();
  });
}

uint32_t script_buttons() { return script().buttons.load(); }

void script_on_log(const std::string& line) {
  Script& s = script();
  if (!s.waiting || s.waiting_draws || line.rfind("script:", 0) == 0) return;
  s.note(line, false);
}

void script_on_draw(const void* pipeline, const std::function<std::string()>& describe) {
  Script& s = script();
  if (!s.waiting_draws && !s.tracing) return;
  std::string text;
  {
    std::lock_guard g(s.lock);
    auto it = s.described.find(pipeline);
    if (it != s.described.end()) text = it->second;
  }
  if (text.empty()) {
    text = describe();
    {
      std::lock_guard g(s.lock);
      s.described[pipeline] = text;
    }
    if (s.tracing) logf("script: first draw with %s", text.c_str());
  }
  s.note(text, true);
}

bool script_wants_capture(uint32_t& number) {
  Script& s = script();
  int expected = 1;
  if (!s.capture.compare_exchange_strong(expected, 0)) return false;
  number = ++s.captures;
  return true;
}

}  // namespace am::rt
