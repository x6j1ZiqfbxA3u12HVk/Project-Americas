// Frame pacing: the console's frame interval and frame events (device slots 78-80).
//
//   78  SetFrameIntervalX(output, interval in microseconds, period in intervals, flags): the
//       game asks for 33,333 us, its 30 fps;
//   79  ScheduleFrameEventX(event type, offset in microseconds, ?, flags);
//   80  WaitFrameEventX(event type, timeout in ms, ?, flags, &frame token): the game's render
//       loop waits here once a frame and numbers its frames by the token.
//
// Frames start at multiples of the interval from the first wait. Each event type keeps the last
// frame it returned: a wait returns at the start of the next one, or at once with the latest
// frame when that has begun already (a title slower than its interval does not wait at all).
// The game waits for two event types each frame, so both see every frame. Offsets are kept but
// all events fire at the frame start for now (no display: nothing else to line up with).

#include <windows.h>

#include <atomic>
#include <mutex>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

class FramePacer {
 public:
  void set_interval(uint32_t microseconds, uint32_t period) {
    std::lock_guard g(lock_);
    if (microseconds) interval_us_ = microseconds;
    period_ = period;
  }

  uint64_t wait(uint32_t type) {
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    uint64_t frame = 0;
    int64_t due = 0;
    {
      std::lock_guard g(lock_);
      const int64_t ticks = static_cast<int64_t>(interval_us_) * f.QuadPart / 1000000;
      if (!start_) start_ = now.QuadPart;
      const uint64_t begun = static_cast<uint64_t>((now.QuadPart - start_) / ticks);
      uint64_t& last = last_[type % kTypes];
      frame = last + 1;
      if (frame <= begun) frame = begun;  // late: the latest frame, no waiting
      last = frame;
      due = start_ + static_cast<int64_t>(frame) * ticks;
    }
    sleep_until(due, f.QuadPart);
    return frame;
  }

 private:
  // A high-resolution timer for most of the wait, then a short spin for the rest.
  void sleep_until(int64_t due, int64_t frequency) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t left = due - now.QuadPart;
    if (left <= 0) return;
    const int64_t spin = frequency / 2000;  // the last half millisecond
    if (timer && left > spin) {
      LARGE_INTEGER relative;
      relative.QuadPart = -((left - spin) * 10000000 / frequency);  // 100 ns units, negative: relative
      if (SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    do {
      YieldProcessor();
      QueryPerformanceCounter(&now);
    } while (now.QuadPart < due);
  }

  std::mutex lock_;
  uint32_t interval_us_ = 16667;  // until the title sets one
  uint32_t period_ = 1;
  int64_t start_ = 0;
  static constexpr uint32_t kTypes = 8;
  uint64_t last_[kTypes] = {};
};

FramePacer& pacer() {
  static FramePacer* const p = new FramePacer;
  return *p;
}

HRESULT WINAPI set_frame_interval(Object* self, void* output, UINT microseconds, UINT period, UINT flags) {
  trace(self, 78, "SetFrameIntervalX", microseconds, period, flags);
  (void)output;
  pacer().set_interval(microseconds, period);
  return S_OK;
}

HRESULT WINAPI schedule_frame_event(Object* self, UINT type, UINT offset_us, uint64_t arg, UINT flags) {
  trace(self, 79, "ScheduleFrameEventX", type, offset_us, flags);
  (void)arg;
  return S_OK;
}

HRESULT WINAPI wait_frame_event(Object* self, UINT type, UINT timeout_ms, uint64_t arg, UINT flags, uint64_t* token) {
  trace(self, 80, "WaitFrameEventX", type, timeout_ms, flags);
  (void)arg;
  const uint64_t frame = pacer().wait(type);
  if (token) *token = frame;
  return S_OK;
}

}  // namespace

void add_frame_methods(std::vector<Method>& out) {
  out.insert(out.end(), {
      {78, "SetFrameIntervalX", reinterpret_cast<void*>(&set_frame_interval)},
      {79, "ScheduleFrameEventX", reinterpret_cast<void*>(&schedule_frame_event)},
      {80, "WaitFrameEventX", reinterpret_cast<void*>(&wait_frame_event)},
  });
}

}  // namespace am::rt::d3d12x
