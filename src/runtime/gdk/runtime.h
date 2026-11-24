#pragma once

// xgameruntime.dll: the GDK's services (plan, section 5.4).
//
// The title links a static part of the GDK that loads xgameruntime.dll (our pseudo module),
// initializes it (InitializeApiImplEx2) and then, for every call, asks QueryApiImpl for the
// object of an area by two GUIDs (class and interface), calls one slot of its vtable with the
// object first, and releases it. The GUIDs and slots here were read from the game's own 161
// thunks; the names are the GDK's public function names.
//
// An area is offered only once some of its methods exist, so the title's thunks fail cleanly
// for the others. A slot that is not implemented logs its name and fails: E_NOTIMPL, or 0 for
// functions that return a bool or a value.

#include <windows.h>

#include <cstdint>
#include <vector>

#include "core/json.h"

namespace am::rt::hle {
struct Function;
}

namespace am::rt::gdk {

// "073b7dcb-1fcf-4030-94be-e3c9eb623428" as a GUID, at compile time.
constexpr GUID guid(const char (&s)[37]) {
  auto nibble = [](char c) -> uint32_t {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
  };
  auto hex = [&](int at, int digits) -> uint32_t {
    uint32_t v = 0;
    for (int i = 0; i < digits; ++i) v = v << 4 | nibble(s[at + i]);
    return v;
  };
  GUID g{};
  g.Data1 = hex(0, 8);
  g.Data2 = static_cast<uint16_t>(hex(9, 4));
  g.Data3 = static_cast<uint16_t>(hex(14, 4));
  const int at[8] = {19, 21, 24, 26, 28, 30, 32, 34};
  for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<uint8_t>(hex(at[i], 2));
  return g;
}

enum class Returns : uint8_t { Hresult, Zero };  // Zero: bool, a value, or nothing

struct Method {
  int slot;
  const char* name;
  void* function;  // nullptr: not implemented yet
  Returns returns = Returns::Hresult;
};

struct Interface {
  const char* area;
  GUID cls;
  GUID iid;
  std::vector<Method> methods;
};

#define AM_GDK(fn) reinterpret_cast<void*>(&fn)

// Each area adds its interfaces.
void add_task_queue_interfaces(std::vector<Interface>& out);
void add_system_interfaces(std::vector<Interface>& out);

// The xgameruntime.dll exports, for the import registry.
void add_runtime_exports(std::vector<hle::Function>& out);

// For report.json: interfaces asked for, and the missing methods the title called.
void write_runtime_report(JsonWriter& j);

// QueryApiImpl, for tests.
HRESULT query_api(const GUID* cls, const GUID* iid, void** out);

}  // namespace am::rt::gdk
