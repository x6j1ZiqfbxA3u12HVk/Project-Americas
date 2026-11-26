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
#include <unordered_set>
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
  enum Kind { Tap, Wait, Log, Draw, Capture, Trace, Quit, Settle } kind = Wait;
  uint32_t button = 0;  // Tap
  int count = 1;        // Tap: how many times; with `until`, until the condition holds
  bool until = false;   // Tap until a Log or Draw condition
  bool until_draw = false;
  bool next = false;  // Tap: wait for the page to change and settle after each tap
  double most = 8;    // Tap with `next`, Settle: the longest wait, in seconds
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
  if (!_stricmp(word.c_str(), "settle")) {
    step.kind = Step::Settle;
    if (!rest.empty()) step.most = std::atof(rest.c_str());
    return step.most > 0;
  }
  step.kind = Step::Tap;
  step.button = button_of(word);
  if (!step.button) return false;
  // "next" or "next <seconds>", last
  {
    std::istringstream words(rest);
    std::vector<std::string> w;
    for (std::string x; words >> x;) w.push_back(x);
    size_t at = w.size();
    if (at >= 1 && !_stricmp(w[at - 1].c_str(), "next")) {
      at -= 1;
    } else if (at >= 2 && !_stricmp(w[at - 2].c_str(), "next") && std::atof(w[at - 1].c_str()) > 0) {
      step.most = std::atof(w[at - 1].c_str());
      at -= 2;
    }
    if (at < w.size()) {
      step.next = true;
      rest.clear();
      for (size_t k = 0; k < at; ++k) rest += (k ? " " : "") + w[k];
      if (rest.empty()) return true;
    }
  }
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

  // What a frame draws, while a step watches the page: the set of its pipelines, as one number.
  std::atomic<bool> watching{false};
  std::unordered_set<uint64_t> drawn;                               // this frame's draws (under `lock`)
  std::atomic<uint64_t> signature{0}, signature_at{0}, frames{0};  // the last frame's, since when
  std::atomic<uint64_t> loaded_at{0};                               // the last storage read

  void on_draw(uint64_t draw) {
    std::lock_guard g(lock);
    drawn.insert(draw);
  }
  void on_frame() {
    uint64_t sig = 0;
    {
      std::lock_guard g(lock);
      for (const uint64_t d : drawn) {
        const uint64_t h = d * 0x9E3779B97F4A7C15ull;
        sig += h ^ (h >> 29);
      }
      drawn.clear();
    }
    if (sig != signature.exchange(sig)) signature_at = GetTickCount64();
    ++frames;
  }

  // Waits for the page to settle: its frames' pipelines unchanged and no storage reads for a
  // while. After a tap, first for the page to change (from `before`, the signature then).
  void settle(size_t step, bool after_tap, uint64_t before, uint64_t tapped, double most) {
    constexpr uint64_t kQuiet = 350, kChange = 2000;
    const uint64_t kMost = static_cast<uint64_t>(most * 1000);
    bool changed = !after_tap;
    for (;;) {
      Sleep(20);
      const uint64_t now = GetTickCount64();
      if (!changed && (signature.load() != before || loaded_at.load() > tapped)) changed = true;
      if (!changed && now - tapped < kChange) continue;
      const bool quiet = now - signature_at.load() >= kQuiet && now - loaded_at.load() >= kQuiet;
      if (quiet || now - tapped >= kMost) {
        logf("script: step %zu %s in %.2f s%s", step, quiet ? "settled" : "did not settle", (now - tapped) / 1000.0,
             changed ? "" : " (the page did not change)");
        return;
      }
    }
  }

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
        case Step::Settle:
          watching = true;
          Sleep(100);
          settle(i + 1, false, 0, GetTickCount64(), s.most);
          watching = false;
          break;
        case Step::Tap:
          if (s.next) {
            watching = true;
            const uint64_t f0 = frames.load();
            while (frames.load() < f0 + 2) Sleep(10);  // a whole frame's set to compare with
            for (int k = 0; k < s.count; ++k) {
              const uint64_t before = signature.load(), tapped = GetTickCount64();
              tap(s.button);
              settle(i + 1, true, before, tapped, s.most);
            }
            watching = false;
          } else if (!s.until) {
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

void script_on_draw(const void* pipeline, const std::function<std::string()>& describe, uint64_t shape) {
  Script& s = script();
  if (s.watching) s.on_draw(reinterpret_cast<uint64_t>(pipeline) ^ (shape * 0xC2B2AE3D27D4EB4Full));
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

void script_on_frame() {
  Script& s = script();
  if (s.watching) s.on_frame();
}

void script_on_load() { script().loaded_at.store(GetTickCount64(), std::memory_order_relaxed); }

bool script_wants_capture(uint32_t& number) {
  Script& s = script();
  int expected = 1;
  if (!s.capture.compare_exchange_strong(expected, 0)) return false;
  number = ++s.captures;
  return true;
}

}  // namespace am::rt
