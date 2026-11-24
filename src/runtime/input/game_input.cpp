// GameInput.dll: GameInputCreate (the game imports it as ordinal 1) and IGameInput, the public
// GameInput API's interface. The game calls GetCurrentReading, GetNextReading,
// GetPreviousReading, RegisterDeviceCallback, UnregisterCallback and FindDeviceFromId.
//
// For now there are no devices: no readings, no device callbacks. Pads (XInput, or the host's
// GameInput) come with M2's input step (plan, section 5.9).

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::input {
namespace {

constexpr HRESULT kDeviceNotFound = static_cast<HRESULT>(0x838A0002);   // GAMEINPUT_E_DEVICE_NOT_FOUND
constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838A0003);  // GAMEINPUT_E_READING_NOT_FOUND

struct GameInput {
  void* const* vtable;
  std::atomic<long> refs;
};

std::atomic<uint64_t> g_next_token{1};

HRESULT WINAPI query_interface(GameInput* self, const GUID*, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG WINAPI add_ref(GameInput* self) { return static_cast<ULONG>(self->refs.fetch_add(1) + 1); }
ULONG WINAPI release(GameInput* self) { return static_cast<ULONG>(self->refs.fetch_sub(1) - 1); }  // static object

// Microseconds, as GameInput's timestamps are.
uint64_t WINAPI get_current_timestamp(GameInput*) {
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(now.QuadPart / frequency.QuadPart * 1000000 +
                               now.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}
HRESULT WINAPI get_current_reading(GameInput*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return kReadingNotFound;
}
HRESULT WINAPI get_next_reading(GameInput*, void*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return kReadingNotFound;
}
HRESULT WINAPI get_temporal_reading(GameInput*, uint64_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return kReadingNotFound;
}
HRESULT WINAPI register_reading_callback(GameInput*, void*, uint32_t, float, void*, void*, uint64_t* token) {
  if (!token) return E_POINTER;
  *token = g_next_token.fetch_add(1);
  return S_OK;
}
// No devices: a blocking enumeration finds none, and none connect later.
HRESULT WINAPI register_device_callback(GameInput*, void*, uint32_t kind, uint32_t, uint32_t, void*, void*,
                                        uint64_t* token) {
  if (!token) return E_POINTER;
  *token = g_next_token.fetch_add(1);
  logf("GameInput: device callback for kinds 0x%x registered; no devices yet", kind);
  return S_OK;
}
HRESULT WINAPI register_other_callback(GameInput*, void*, uint32_t, void*, void*, uint64_t* token) {
  if (!token) return E_POINTER;
  *token = g_next_token.fetch_add(1);
  return S_OK;
}
HRESULT WINAPI register_keyboard_layout_callback(GameInput*, void*, void*, void*, uint64_t* token) {
  if (!token) return E_POINTER;
  *token = g_next_token.fetch_add(1);
  return S_OK;
}
void WINAPI stop_callback(GameInput*, uint64_t) {}
bool WINAPI unregister_callback(GameInput*, uint64_t, uint64_t) { return true; }
HRESULT WINAPI not_implemented(GameInput*, void*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}
HRESULT WINAPI create_aggregate_device(GameInput*, uint32_t, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}
HRESULT WINAPI find_device(GameInput*, const void*, void** out) {
  if (out) *out = nullptr;
  return kDeviceNotFound;
}
HRESULT WINAPI enable_oem_device_support(GameInput*, uint16_t, uint16_t, uint8_t, uint8_t) { return S_OK; }
void WINAPI set_focus_policy(GameInput*, uint32_t) {}

#define AM_SLOT(fn) reinterpret_cast<void*>(&fn)
void* const kVtable[] = {
    AM_SLOT(query_interface),       AM_SLOT(add_ref),
    AM_SLOT(release),               AM_SLOT(get_current_timestamp),
    AM_SLOT(get_current_reading),   AM_SLOT(get_next_reading),
    AM_SLOT(get_next_reading),      // GetPreviousReading: the same arguments
    AM_SLOT(get_temporal_reading),  AM_SLOT(register_reading_callback),
    AM_SLOT(register_device_callback), AM_SLOT(register_other_callback),  // RegisterSystemButtonCallback
    AM_SLOT(register_keyboard_layout_callback), AM_SLOT(stop_callback),
    AM_SLOT(unregister_callback),   AM_SLOT(not_implemented),  // CreateDispatcher
    AM_SLOT(create_aggregate_device), AM_SLOT(find_device),     // FindDeviceFromId
    AM_SLOT(find_device),           AM_SLOT(find_device),       // FromObject, FromPlatformHandle
    AM_SLOT(find_device),           AM_SLOT(enable_oem_device_support),  // FromPlatformString
    AM_SLOT(set_focus_policy),
};
#undef AM_SLOT
static_assert(sizeof kVtable / sizeof kVtable[0] == 22);

GameInput g_game_input{kVtable, 1};

HRESULT WINAPI my_GameInputCreate(GameInput** out) {
  if (!out) return E_POINTER;
  g_game_input.refs.fetch_add(1);
  *out = &g_game_input;
  return S_OK;
}

}  // namespace

void add_input(std::vector<hle::Function>& out) {
  out.push_back(hle::Function{"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&my_GameInputCreate)});
  out.push_back(hle::Function{"gameinput.dll", "#1", reinterpret_cast<void*>(&my_GameInputCreate)});
}

}  // namespace am::rt::input
