// Command allocators and command lists, null back end: commands are accepted and dropped.
//
// The console's command list is partly filled by the title's own code, which writes records
// into the list object's fields at fixed offsets. The object therefore reserves a block of
// console-layout fields after its vtable, zeroed; which fields the game uses shows in the
// probes and, if one is needed, in a crash on its offset.

#include <windows.h>

#include <cstring>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

// ---- allocator ----

struct Allocator : Object {
  uint32_t type = 0;
};

HRESULT WINAPI allocator_reset(Allocator* self) {
  trace(self, 8, "Reset", 0, 0, 0);
  return S_OK;
}

const Kind& allocator_kind() {
  static const Kind k("command allocator", 32, {
      {8, "Reset", reinterpret_cast<void*>(&allocator_reset)},
  });
  return k;
}

// ---- command list ----

constexpr size_t kConsoleFields = 0x1000;

struct CommandList : Object {
  uint8_t console[kConsoleFields - sizeof(void*)] = {};  // the console object's fields, as the title sees them
  uint32_t desc[4] = {};
  Object* allocator = nullptr;
};

const Kind& list_kind() {
  static const Kind k("command list", kMaxSlots, {});
  return k;
}

}  // namespace

HRESULT create_allocator(uint32_t type, const GUID* riid, void** out) {
  if (!out) return E_POINTER;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) logf("d3d12x: CreateCommandAllocator(type %u, iid %s)", type, riid ? guid_text(*riid).c_str() : "null");
  Allocator* a = make_object<Allocator>(allocator_kind());
  a->type = type;
  *out = a;
  return S_OK;
}

HRESULT create_command_list(const uint32_t* desc, Object* allocator, Object* pipeline, const GUID* riid, void** out) {
  if (!out) return E_POINTER;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    logf("d3d12x: CreateCommandListX(desc %08x %08x %08x %08x, allocator %p, pipeline %p, iid %s)", desc ? desc[0] : 0,
         desc ? desc[1] : 0, desc ? desc[2] : 0, desc ? desc[3] : 0, static_cast<void*>(allocator),
         static_cast<void*>(pipeline), riid ? guid_text(*riid).c_str() : "null");
  }
  CommandList* l = make_object<CommandList>(list_kind());
  if (desc) std::memcpy(l->desc, desc, sizeof l->desc);
  l->allocator = allocator;
  *out = l;
  return S_OK;
}

}  // namespace am::rt::d3d12x
