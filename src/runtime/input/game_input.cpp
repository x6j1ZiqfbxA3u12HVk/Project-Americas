// GameInput.dll: GameInputCreate (the game imports it as ordinal 1) and the objects of the public
// GameInput API: IGameInput, IGameInputDevice, IGameInputReading.
//
// Devices:
//   0-3  the host's XInput pads, by slot;
//   4    a stand-in pad the keyboard drives, connected while no XInput pad is (keys below);
//   5    the keyboard.
// A poll thread (every 4 ms) reads them; each change of a device's state is a new reading,
// kept in a short history per device (the game takes the current reading and walks back to the
// previous one). Device callbacks run when a device connects or disconnects, and at
// registration for the devices already there (blocking and asynchronous enumeration alike).
// Keys count only while one of our windows has the focus.
//
// AMERICAS_PRESS (testing without hands): "A:45,MENU:60" presses those stand-in pad buttons
// for 200 ms at those seconds after input starts (A B X Y MENU VIEW UP DOWN LEFT RIGHT).
//
// The stand-in pad's keys: A Enter / Space, B Escape / Backspace, X the X key, Y the Y key,
// Menu Tab, View V, the d-pad the arrow keys, shoulders Q and E, stick clicks F and G, the left
// stick W A S D, the right stick I J K L, triggers Z and C.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/hle/registry.h"
#include "runtime/log.h"
#include "runtime/script.h"

namespace am::rt::input {
namespace {

constexpr HRESULT kDisconnected = static_cast<HRESULT>(0x838A0001);      // GAMEINPUT_E_DEVICE_DISCONNECTED
constexpr HRESULT kDeviceNotFound = static_cast<HRESULT>(0x838A0002);    // GAMEINPUT_E_DEVICE_NOT_FOUND
constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838A0003);   // GAMEINPUT_E_READING_NOT_FOUND
constexpr HRESULT kReferenceTooOld = static_cast<HRESULT>(0x838A0004);   // GAMEINPUT_E_REFERENCE_READING_TOO_OLD

constexpr uint32_t kKeyboardKind = 0x10, kGamepadKind = 0x40000;
constexpr uint32_t kStatusConnected = 0x3;  // connected, input enabled

constexpr size_t kPads = 4, kStandIn = 4, kKeyboard = 5, kDevices = 6;
constexpr size_t kHistory = 32;

// Gamepad buttons, as GameInput numbers them.
constexpr uint32_t kMenu = 0x1, kView = 0x2, kA = 0x4, kB = 0x8, kX = 0x10, kY = 0x20, kUp = 0x40, kDown = 0x80,
                   kLeft = 0x100, kRight = 0x200, kLeftShoulder = 0x400, kRightShoulder = 0x800, kLeftStick = 0x1000,
                   kRightStick = 0x2000;

struct PadState {
  uint32_t buttons = 0;
  float left_trigger = 0, right_trigger = 0, left_x = 0, left_y = 0, right_x = 0, right_y = 0;
  bool operator==(const PadState&) const = default;
};
struct KeyState {
  uint32_t scan_code = 0, code_point = 0;
  uint8_t virtual_key = 0;
  bool dead_key = false;
  bool operator==(const KeyState&) const = default;
};
static_assert(sizeof(PadState) == 28 && sizeof(KeyState) == 12);

// GameInputDeviceInfo: the game reads the device id at +0x20; the rest is the public layout,
// zero where we have nothing to say.
struct DeviceInfo {
  uint32_t info_size;
  uint16_t vendor, product, revision;
  uint8_t interface_number, collection_number;
  uint16_t usage_page, usage_id;
  uint16_t versions[8];
  uint8_t id[32], root_id[32];
  int32_t family;
  uint32_t capabilities, supported_input, supported_rumble_motors;
  uint64_t rest[26];
};
static_assert(offsetof(DeviceInfo, id) == 0x20);

struct Device {
  void* const* vtable;
  size_t index;
  DeviceInfo info;
};

struct Reading {
  void* const* vtable;
  std::atomic<long> refs{1};
  Device* device = nullptr;
  uint64_t sequence = 0, timestamp = 0;
  uint32_t kind = 0;
  PadState pad;
  std::vector<KeyState> keys;
};

void reading_add_ref(Reading* r) { r->refs.fetch_add(1, std::memory_order_relaxed); }
void reading_release(Reading* r) {
  if (r->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete r;
}

using DeviceCallback = void(WINAPI*)(uint64_t token, void* context, Device* device, uint64_t timestamp, uint32_t status,
                                     uint32_t previous);
struct Registration {
  uint64_t token;
  Device* device;  // null: any
  uint32_t kind, status;
  void* context;
  DeviceCallback callback;
};
struct Event {
  Registration r;
  Device* device;
  uint64_t timestamp;
  uint32_t status, previous;
};

uint64_t now_us() {
  static const int64_t f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return q.QuadPart;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart / f * 1000000 + t.QuadPart % f * 1000000 / f);
}

// ---- the host's input ----

struct XInputGamepad {
  WORD buttons;
  BYTE left_trigger, right_trigger;
  SHORT lx, ly, rx, ry;
};
struct XInputState {
  DWORD packet;
  XInputGamepad pad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);

PadState from_xinput(const XInputGamepad& g) {
  struct Bit {
    WORD bit;
    uint32_t button;
  };
  static const Bit kMap[] = {
      {0x0010, kMenu}, {0x0020, kView},  {0x1000, kA},          {0x2000, kB},           {0x4000, kX},
      {0x8000, kY},    {0x0001, kUp},    {0x0002, kDown},       {0x0004, kLeft},        {0x0008, kRight},
      {0x0100, kLeftShoulder}, {0x0200, kRightShoulder}, {0x0040, kLeftStick}, {0x0080, kRightStick},
  };
  PadState p;
  for (const Bit& b : kMap) {
    if (g.buttons & b.bit) p.buttons |= b.button;
  }
  auto axis = [](SHORT v) {
    const float f = std::max(-1.0f, static_cast<float>(v) / 32767.0f);
    return std::abs(f) < 0.12f ? 0.0f : f;  // XInput's suggested dead zone, about
  };
  p.left_trigger = g.left_trigger / 255.0f;
  p.right_trigger = g.right_trigger / 255.0f;
  p.left_x = axis(g.lx);
  p.left_y = axis(g.ly);
  p.right_x = axis(g.rx);
  p.right_y = axis(g.ry);
  return p;
}

bool our_window_has_focus() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

std::vector<KeyState> held_keys() {
  std::vector<KeyState> keys;
  if (!our_window_has_focus()) return keys;
  for (int vk = 8; vk < 255; ++vk) {
    if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;  // the left / right keys come too
    KeyState k;
    k.virtual_key = static_cast<uint8_t>(vk);
    k.scan_code = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    keys.push_back(k);
  }
  return keys;
}

PadState stand_in(const std::vector<KeyState>& keys) {
  auto on = [&](int vk) { return std::any_of(keys.begin(), keys.end(), [vk](const KeyState& k) { return k.virtual_key == vk; }); };
  auto level = [&](int vk) { return on(vk) ? 1.0f : 0.0f; };
  struct Key {
    int vk;
    uint32_t button;
  };
  static const Key kMap[] = {
      {VK_RETURN, kA}, {VK_SPACE, kA}, {VK_ESCAPE, kB}, {VK_BACK, kB},     {'X', kX},          {'Y', kY},
      {VK_TAB, kMenu}, {'V', kView},   {VK_UP, kUp},    {VK_DOWN, kDown}, {VK_LEFT, kLeft},   {VK_RIGHT, kRight},
      {'Q', kLeftShoulder}, {'E', kRightShoulder}, {'F', kLeftStick}, {'G', kRightStick},
  };
  PadState p;
  for (const Key& k : kMap) {
    if (on(k.vk)) p.buttons |= k.button;
  }
  p.left_trigger = level('Z');
  p.right_trigger = level('C');
  p.left_x = level('D') - level('A');
  p.left_y = level('W') - level('S');
  p.right_x = level('L') - level('J');
  p.right_y = level('I') - level('K');
  return p;
}

// AMERICAS_PRESS: the buttons scripted to be down now.
uint32_t scripted_buttons(uint64_t since_start_us) {
  struct Press {
    uint32_t button;
    uint64_t at_us;
  };
  static const std::vector<Press> presses = [] {
    std::vector<Press> out;
    char text[512] = {};
    if (!GetEnvironmentVariableA("AMERICAS_PRESS", text, sizeof text)) return out;
    static const struct {
      const char* name;
      uint32_t button;
    } kNames[] = {{"A", kA},     {"B", kB},       {"X", kX},       {"Y", kY},       {"MENU", kMenu},
                  {"VIEW", kView}, {"UP", kUp},   {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight}};
    char* rest = nullptr;
    for (char* item = strtok_s(text, ",", &rest); item; item = strtok_s(nullptr, ",", &rest)) {
      char* colon = std::strchr(item, ':');
      if (!colon) continue;
      *colon = 0;
      for (const auto& n : kNames) {
        if (!_stricmp(n.name, item)) out.push_back({n.button, static_cast<uint64_t>(std::atof(colon + 1) * 1e6)});
      }
    }
    logf("GameInput: %zu scripted presses (AMERICAS_PRESS)", out.size());
    return out;
  }();
  uint32_t down = 0;
  for (const Press& p : presses) {
    if (since_start_us >= p.at_us && since_start_us < p.at_us + 200000) down |= p.button;
  }
  return down;
}

// ---- the hub: devices, their readings, callbacks ----

extern void* const kDeviceVtable[];
extern void* const kReadingVtable[];

class Hub {
 public:
  Hub() {
    for (size_t i = 0; i < kDevices; ++i) {
      Device& d = devices_[i];
      d.vtable = kDeviceVtable;
      d.index = i;
      std::memset(&d.info, 0, sizeof d.info);
      d.info.info_size = sizeof(DeviceInfo);
      static const char kTag[] = "americas-input";
      std::memcpy(d.info.id, kTag, sizeof kTag - 1);
      d.info.id[31] = static_cast<uint8_t>(i + 1);
      std::memcpy(d.info.root_id, d.info.id, sizeof d.info.id);
      d.info.supported_input = i == kKeyboard ? kKeyboardKind : kGamepadKind;
      d.info.supported_rumble_motors = i < kPads ? 0x3 : 0;
    }
    if (HMODULE x = LoadLibraryW(L"xinput1_4.dll")) xinput_ = reinterpret_cast<XInputGetStateFn>(GetProcAddress(x, "XInputGetState"));
  }

  void start() {
    std::call_once(started_, [this] {
      poll();  // the devices there are now, before the first registration enumerates them
      std::thread([this] {
        for (;;) {
          Sleep(4);
          poll();
        }
      }).detach();
    });
  }

  Device* device(size_t i) { return &devices_[i]; }
  bool ours(const Device* d) const { return d >= devices_ && d < devices_ + kDevices; }

  HRESULT current(uint32_t kind, Device* device, Reading** out) {
    std::lock_guard g(lock_);
    Reading* found = nullptr;
    if (device) {
      if (!ours(device)) return fail(out, E_INVALIDARG);
      if (!connected_[device->index]) return fail(out, kDisconnected);
      found = newest(device->index, kind);
    } else {
      for (size_t i = 0; i < kDevices; ++i) {
        Reading* r = connected_[i] ? newest(i, kind) : nullptr;
        if (r && (!found || r->timestamp > found->timestamp)) found = r;
      }
    }
    return hand(out, found);
  }

  HRESULT relative(Reading* reference, uint32_t kind, Device* device, Reading** out, bool forward) {
    if (!reference) return fail(out, E_INVALIDARG);
    std::lock_guard g(lock_);
    Device* d = reference->device;
    if (!ours(d) || (device && device != d)) return fail(out, E_INVALIDARG);
    const auto& h = history_[d->index];
    if (h.empty() || reference->sequence < h.front()->sequence) return fail(out, kReferenceTooOld);
    Reading* found = nullptr;
    if (forward) {
      for (Reading* r : h) {
        if (r->sequence > reference->sequence && (r->kind & kind)) {
          found = r;
          break;
        }
      }
    } else {
      for (auto it = h.rbegin(); it != h.rend(); ++it) {
        if ((*it)->sequence < reference->sequence && ((*it)->kind & kind)) {
          found = *it;
          break;
        }
      }
    }
    return hand(out, found);
  }

  HRESULT temporal(uint64_t time, Device* device, Reading** out) {
    std::lock_guard g(lock_);
    if (!ours(device)) return fail(out, E_INVALIDARG);
    const auto& h = history_[device->index];
    for (auto it = h.rbegin(); it != h.rend(); ++it) {
      if ((*it)->timestamp <= time) return hand(out, *it);
    }
    return hand(out, nullptr);
  }

  uint64_t register_device(Device* device, uint32_t kind, uint32_t status, uint32_t enumeration, void* context,
                           DeviceCallback callback) {
    start();
    Registration r{next_token_.fetch_add(1), device, kind, status, context, callback};
    std::vector<Event> events;
    {
      std::lock_guard g(lock_);
      if (enumeration && (status & kStatusConnected)) {
        const uint64_t t = now_us();
        for (size_t i = 0; i < kDevices; ++i) {
          if (connected_[i] && matches(r, i)) events.push_back({r, &devices_[i], t, kStatusConnected, 0});
        }
      }
      registrations_.push_back(r);
    }
    logf("GameInput: device callback for kinds 0x%x (enumeration %u): %zu devices now", kind, enumeration, events.size());
    deliver(events);
    return r.token;
  }

  bool unregister(uint64_t token) {
    std::lock_guard g(lock_);
    const size_t before = registrations_.size();
    std::erase_if(registrations_, [token](const Registration& r) { return r.token == token; });
    return registrations_.size() != before;
  }

  uint32_t status(const Device* d) {
    std::lock_guard g(lock_);
    return connected_[d->index] ? kStatusConnected : 0;
  }

  uint64_t next_token() { return next_token_.fetch_add(1); }

 private:
  static HRESULT fail(Reading** out, HRESULT hr) {
    if (out) *out = nullptr;
    return hr;
  }
  static HRESULT hand(Reading** out, Reading* r) {
    if (!out) return E_POINTER;
    *out = r;
    if (!r) return kReadingNotFound;
    reading_add_ref(r);
    return S_OK;
  }
  Reading* newest(size_t i, uint32_t kind) {
    const auto& h = history_[i];
    return !h.empty() && (h.back()->kind & kind) ? h.back() : nullptr;
  }
  bool matches(const Registration& r, size_t i) const {
    return (!r.device || r.device == &devices_[i]) && (devices_[i].info.supported_input & r.kind);
  }

  void deliver(const std::vector<Event>& events) {
    for (const Event& e : events) e.r.callback(e.r.token, e.r.context, e.device, e.timestamp, e.status, e.previous);
  }

  void add_reading(size_t i, const PadState* pad, const std::vector<KeyState>* keys, uint64_t t) {
    Reading* r = new Reading;
    r->vtable = kReadingVtable;
    r->device = &devices_[i];
    r->sequence = ++sequence_;
    r->timestamp = t;
    if (pad) {
      r->kind = kGamepadKind;
      r->pad = *pad;
    } else {
      r->kind = kKeyboardKind;
      r->keys = *keys;
    }
    auto& h = history_[i];
    h.push_back(r);
    if (h.size() > kHistory) {
      reading_release(h.front());
      h.pop_front();
    }
  }

  void poll() {
    bool now_connected[kDevices] = {};
    PadState pads[kDevices];
    for (size_t i = 0; xinput_ && i < kPads; ++i) {
      XInputState s{};
      if (xinput_(static_cast<DWORD>(i), &s) == ERROR_SUCCESS) {
        now_connected[i] = true;
        pads[i] = from_xinput(s.pad);
      }
    }
    const std::vector<KeyState> keys = held_keys();
    now_connected[kStandIn] = !std::any_of(now_connected, now_connected + kPads, [](bool c) { return c; });
    pads[kStandIn] = stand_in(keys);
    if (!start_us_) start_us_ = now_us();
    script_start();
    pads[kStandIn].buttons |= scripted_buttons(now_us() - start_us_) | script_buttons();
    now_connected[kKeyboard] = true;

    std::vector<Event> events;
    const uint64_t t = now_us();
    {
      std::lock_guard g(lock_);
      for (size_t i = 0; i < kDevices; ++i) {
        if (now_connected[i] != connected_[i]) {
          const uint32_t status = now_connected[i] ? kStatusConnected : 0, previous = connected_[i] ? kStatusConnected : 0;
          for (const Registration& r : registrations_) {
            if (matches(r, i) && (r.status & kStatusConnected)) events.push_back({r, &devices_[i], t, status, previous});
          }
          connected_[i] = now_connected[i];
          if (connected_[i]) {
            logf("GameInput: %s connected", i == kKeyboard ? "the keyboard" : i == kStandIn ? "the keyboard's stand-in pad" : "an XInput pad");
          }
        }
        if (!connected_[i]) continue;
        const auto& h = history_[i];
        if (i == kKeyboard) {
          if (h.empty() || h.back()->keys != keys) add_reading(i, nullptr, &keys, t);
        } else if (h.empty() || !(h.back()->pad == pads[i])) {
          add_reading(i, &pads[i], nullptr, t);
        }
      }
    }
    deliver(events);
  }

  std::mutex lock_;
  std::once_flag started_;
  Device devices_[kDevices];
  bool connected_[kDevices] = {};
  std::deque<Reading*> history_[kDevices];
  std::vector<Registration> registrations_;
  uint64_t sequence_ = 0;
  uint64_t start_us_ = 0;
  std::atomic<uint64_t> next_token_{1};
  XInputGetStateFn xinput_ = nullptr;
};

Hub& hub() {
  static Hub* const h = new Hub;
  return *h;
}

// ---- IGameInput ----

struct GameInput {
  void* const* vtable;
};

HRESULT WINAPI query_interface(void* self, const GUID*, void** out) {
  if (!out) return E_POINTER;
  *out = self;
  return S_OK;
}
ULONG WINAPI static_ref(void*) { return 1; }  // the input object and the devices live for good

uint64_t WINAPI get_current_timestamp(GameInput*) { return now_us(); }
HRESULT WINAPI get_current_reading(GameInput*, uint32_t kind, Device* device, Reading** out) {
  hub().start();
  return hub().current(kind, device, out);
}
HRESULT WINAPI get_next_reading(GameInput*, Reading* reference, uint32_t kind, Device* device, Reading** out) {
  return hub().relative(reference, kind, device, out, true);
}
HRESULT WINAPI get_previous_reading(GameInput*, Reading* reference, uint32_t kind, Device* device, Reading** out) {
  return hub().relative(reference, kind, device, out, false);
}
HRESULT WINAPI get_temporal_reading(GameInput*, uint64_t time, Device* device, Reading** out) {
  return hub().temporal(time, device, out);
}
// Reading callbacks never run: the game polls.
HRESULT WINAPI register_reading_callback(GameInput*, void*, uint32_t, float, void*, void*, uint64_t* token) {
  if (!token) return E_POINTER;
  *token = hub().next_token();
  return S_OK;
}
HRESULT WINAPI register_device_callback(GameInput*, Device* device, uint32_t kind, uint32_t status, uint32_t enumeration,
                                        void* context, DeviceCallback callback, uint64_t* token) {
  if (!callback) return E_INVALIDARG;
  const uint64_t t = hub().register_device(device, kind, status, enumeration, context, callback);
  if (token) *token = t;
  return S_OK;
}
// The guide button and the keyboard layout never change here.
HRESULT WINAPI register_other_callback(GameInput*, void*, void*, void*, uint64_t* token) {
  if (!token) return E_POINTER;
  *token = hub().next_token();
  return S_OK;
}
void WINAPI stop_callback(GameInput*, uint64_t) {}
bool WINAPI unregister_callback(GameInput*, uint64_t token, uint64_t) { return hub().unregister(token); }
HRESULT WINAPI create_dispatcher(GameInput*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}
HRESULT WINAPI create_aggregate_device(GameInput*, uint32_t, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}
HRESULT WINAPI find_device_from_id(GameInput*, const uint8_t* id, Device** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!id) return E_INVALIDARG;
  for (size_t i = 0; i < kDevices; ++i) {
    if (!std::memcmp(hub().device(i)->info.id, id, 32)) {
      *out = hub().device(i);
      return S_OK;
    }
  }
  return kDeviceNotFound;
}
HRESULT WINAPI find_device(GameInput*, const void*, void** out) {
  if (out) *out = nullptr;
  return kDeviceNotFound;
}
HRESULT WINAPI enable_oem_device_support(GameInput*, uint16_t, uint16_t, uint8_t, uint8_t) { return S_OK; }
void WINAPI set_focus_policy(GameInput*, uint32_t) {}

#define AM_SLOT(fn) reinterpret_cast<void*>(&fn)
void* const kInputVtable[] = {
    AM_SLOT(query_interface),           AM_SLOT(static_ref),
    AM_SLOT(static_ref),                AM_SLOT(get_current_timestamp),
    AM_SLOT(get_current_reading),       AM_SLOT(get_next_reading),
    AM_SLOT(get_previous_reading),      AM_SLOT(get_temporal_reading),
    AM_SLOT(register_reading_callback), AM_SLOT(register_device_callback),
    AM_SLOT(register_other_callback),   AM_SLOT(register_other_callback),  // system button, keyboard layout
    AM_SLOT(stop_callback),             AM_SLOT(unregister_callback),
    AM_SLOT(create_dispatcher),         AM_SLOT(create_aggregate_device),
    AM_SLOT(find_device_from_id),       AM_SLOT(find_device),  // FromObject
    AM_SLOT(find_device),               AM_SLOT(find_device),  // FromPlatformHandle, FromPlatformString
    AM_SLOT(enable_oem_device_support), AM_SLOT(set_focus_policy),
};
static_assert(sizeof kInputVtable / sizeof kInputVtable[0] == 22);

// ---- IGameInputDevice ----

const DeviceInfo* WINAPI device_info(Device* self) { return &self->info; }
uint32_t WINAPI device_status(Device* self) { return hub().status(self); }
// Rumble (four levels: low and high frequency, the triggers) to XInput's two motors.
void WINAPI device_set_rumble(Device* self, const float* levels) {
  if (self->index >= kPads) return;
  static auto set_state = [] {
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");
    return x ? reinterpret_cast<DWORD(WINAPI*)(DWORD, WORD*)>(GetProcAddress(x, "XInputSetState")) : nullptr;
  }();
  if (!set_state) return;
  auto speed = [](float v) { return static_cast<WORD>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
  WORD motors[2] = {levels ? speed(levels[0]) : WORD{0}, levels ? speed(levels[1]) : WORD{0}};
  set_state(static_cast<DWORD>(self->index), motors);
}
HRESULT WINAPI device_unsupported(Device*) { return E_NOTIMPL; }

void* const kDeviceVtable[] = {
    AM_SLOT(query_interface), AM_SLOT(static_ref), AM_SLOT(static_ref), AM_SLOT(device_info), AM_SLOT(device_status),
    AM_SLOT(device_unsupported),  // battery
    AM_SLOT(device_unsupported), AM_SLOT(device_unsupported), AM_SLOT(device_unsupported),  // force feedback
    AM_SLOT(device_unsupported),  // haptics
    AM_SLOT(device_set_rumble),
    AM_SLOT(device_unsupported), AM_SLOT(device_unsupported),  // input synchronization
    AM_SLOT(device_unsupported),                               // power off
    AM_SLOT(device_unsupported), AM_SLOT(device_unsupported), AM_SLOT(device_unsupported),  // raw device access
    AM_SLOT(device_unsupported), AM_SLOT(device_unsupported), AM_SLOT(device_unsupported), AM_SLOT(device_unsupported),
};
static_assert(sizeof kDeviceVtable / sizeof kDeviceVtable[0] == 21);

// ---- IGameInputReading ----

ULONG WINAPI r_add_ref(Reading* self) { return static_cast<ULONG>(self->refs.fetch_add(1) + 1); }
ULONG WINAPI r_release(Reading* self) {
  const long left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}
uint32_t WINAPI r_kind(Reading* self) { return self->kind; }
uint64_t WINAPI r_sequence(Reading* self, uint32_t kind) { return (self->kind & kind) ? self->sequence : 0; }
uint64_t WINAPI r_timestamp(Reading* self) { return self->timestamp; }
void WINAPI r_device(Reading* self, Device** out) {
  if (out) *out = self->device;
}
uint64_t WINAPI r_nothing(Reading*) { return 0; }
uint32_t WINAPI r_key_count(Reading* self) { return static_cast<uint32_t>(self->keys.size()); }
uint32_t WINAPI r_key_state(Reading* self, uint32_t count, KeyState* out) {
  if (!out) return 0;
  const uint32_t n = std::min<uint32_t>(count, static_cast<uint32_t>(self->keys.size()));
  std::copy_n(self->keys.begin(), n, out);
  return n;
}
bool WINAPI r_gamepad_state(Reading* self, PadState* out) {
  if (self->kind != kGamepadKind || !out) return false;
  *out = self->pad;
  return true;
}

void* const kReadingVtable[] = {
    AM_SLOT(query_interface), AM_SLOT(r_add_ref),   AM_SLOT(r_release),     AM_SLOT(r_kind),
    AM_SLOT(r_sequence),      AM_SLOT(r_timestamp), AM_SLOT(r_device),      AM_SLOT(r_nothing),  // raw report
    AM_SLOT(r_nothing),       AM_SLOT(r_nothing),   AM_SLOT(r_nothing),     AM_SLOT(r_nothing),  // controller axes, buttons
    AM_SLOT(r_nothing),       AM_SLOT(r_nothing),                                                // switches
    AM_SLOT(r_key_count),     AM_SLOT(r_key_state),
    AM_SLOT(r_nothing),  // mouse
    AM_SLOT(r_nothing),       AM_SLOT(r_nothing),  // touch
    AM_SLOT(r_nothing),  // motion
    AM_SLOT(r_nothing),       AM_SLOT(r_nothing),  // arcade stick, flight stick
    AM_SLOT(r_gamepad_state),
    AM_SLOT(r_nothing),       AM_SLOT(r_nothing),  // racing wheel, UI navigation
};
static_assert(sizeof kReadingVtable / sizeof kReadingVtable[0] == 25);
#undef AM_SLOT

GameInput g_game_input{kInputVtable};

HRESULT WINAPI my_GameInputCreate(GameInput** out) {
  if (!out) return E_POINTER;
  *out = &g_game_input;
  return S_OK;
}

}  // namespace

void add_input(std::vector<hle::Function>& out) {
  out.push_back(hle::Function{"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&my_GameInputCreate)});
  out.push_back(hle::Function{"gameinput.dll", "#1", reinterpret_cast<void*>(&my_GameInputCreate)});
}

}  // namespace am::rt::input
