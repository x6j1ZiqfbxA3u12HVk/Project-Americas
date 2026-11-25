// Command queues and fences, null back end: work submitted to a queue counts as finished at
// once, so a fence reaches a value as soon as anything signals it.
//
// Slot numbers: D3D12's order where the game's calls agree with it (ID3D12Fence, the start of
// ID3D12CommandQueue); the rest stay probes until the game shows what they are. Implemented slots
// log their first calls too, so a wrong guess shows in the log.

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "runtime/d3d12x/commands.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

// ---- fence ----

// The fence's value is a 64-bit word in memory, as on the console, where the GPU writes it and
// title code reads it directly (slot 12 gives its address).
struct Fence : Object {
  explicit Fence(uint64_t initial) : value(initial) {}
  std::mutex lock;
  std::atomic<uint64_t> value;
  std::vector<std::pair<uint64_t, HANDLE>> waits;  // events to set when the value is reached

  void signal(uint64_t v) {
    std::vector<HANDLE> ready;
    {
      std::lock_guard g(lock);
      value = v;
      for (auto it = waits.begin(); it != waits.end();) {
        if (it->first <= v) {
          ready.push_back(it->second);
          it = waits.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (HANDLE e : ready) SetEvent(e);
  }
};

static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));

UINT64 WINAPI fence_get_completed_value(Fence* self) { return self->value.load(); }

// Slot 12: the address of the fence's value. The game's wait loop reads it until the value it
// waits for arrives.
const volatile uint64_t* WINAPI fence_value_address(Fence* self) {
  trace(self, 12, "value address", reinterpret_cast<uint64_t>(&self->value), 0, 0);
  return reinterpret_cast<const volatile uint64_t*>(&self->value);
}

HRESULT WINAPI fence_set_event_on_completion(Fence* self, UINT64 value, HANDLE event) {
  trace(self, 9, "SetEventOnCompletion", value, reinterpret_cast<uint64_t>(event), 0);
  {
    std::lock_guard g(self->lock);
    if (self->value < value) {
      // Not reached yet: the event is set when something signals the value. A null event asks
      // to block until then; in the null back end nothing is pending, so it returns at once.
      if (event) self->waits.emplace_back(value, event);
      return S_OK;
    }
  }
  if (event) SetEvent(event);
  return S_OK;
}

HRESULT WINAPI fence_signal(Fence* self, UINT64 value) {
  trace(self, 10, "Signal", value, 0, 0);
  self->signal(value);
  return S_OK;
}

const Kind& fence_kind() {
  static const Kind k("fence", 32, {
      {8, "GetCompletedValue", reinterpret_cast<void*>(&fence_get_completed_value)},
      {9, "SetEventOnCompletion", reinterpret_cast<void*>(&fence_set_event_on_completion)},
      {10, "Signal", reinterpret_cast<void*>(&fence_signal)},
      {12, "value address", reinterpret_cast<void*>(&fence_value_address)},
  });
  return k;
}

// ---- command queue ----

struct Queue : Object {
  Queue(const void* desc, size_t size) {
    std::memcpy(this->desc, desc, size < sizeof this->desc ? size : sizeof this->desc);
  }
  uint32_t desc[8] = {};
};

HRESULT WINAPI queue_signal(Queue* self, Object* fence, UINT64 value) {
  trace(self, 16, "Signal", reinterpret_cast<uint64_t>(fence), value, 0);
  if (Object* f = object_of_kind(fence, fence_kind())) static_cast<Fence*>(f)->signal(value);
  return S_OK;
}

HRESULT WINAPI queue_wait(Queue* self, Object* fence, UINT64 value) {
  trace(self, 17, "Wait", reinterpret_cast<uint64_t>(fence), value, 0);
  return S_OK;  // the null back end has nothing to wait for
}

HRESULT WINAPI queue_get_timestamp_frequency(Queue* self, UINT64* frequency) {
  trace(self, 18, "GetTimestampFrequency", reinterpret_cast<uint64_t>(frequency), 0, 0);
  if (!frequency) return E_POINTER;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  *frequency = static_cast<UINT64>(f.QuadPart);
  return S_OK;
}

void WINAPI queue_execute_command_lists(Queue* self, UINT count, void* const* lists) {
  trace(self, 10, "ExecuteCommandLists", count, reinterpret_cast<uint64_t>(lists), 0);
}

// Slot 35: the console's ExecuteCommandLists(count, lists, frame token, flags); the game
// submits its frame's lists here twice a frame.
void WINAPI queue_execute_command_lists_x(Queue* self, UINT count, void* const* lists, uint64_t token, UINT flags) {
  trace(self, 35, "ExecuteCommandListsX", count, token, flags);
  for (UINT i = 0; lists && i < count; ++i) frame_execute(lists[i]);
  submit_lists(count, lists);
}

const Kind& queue_kind() {
  static const Kind k("command queue", 64, {
      {10, "ExecuteCommandLists", reinterpret_cast<void*>(&queue_execute_command_lists)},
      // D3D12's Signal, Wait and GetTimestampFrequency, two slots later than in D3D12: the game
      // signals fences through slot 16 and asks the frequency through 18.
      {16, "Signal", reinterpret_cast<void*>(&queue_signal)},
      {17, "Wait", reinterpret_cast<void*>(&queue_wait)},
      {18, "GetTimestampFrequency", reinterpret_cast<void*>(&queue_get_timestamp_frequency)},
      {32, "PresentX", reinterpret_cast<void*>(&queue_present_x)},
      {35, "ExecuteCommandListsX", reinterpret_cast<void*>(&queue_execute_command_lists_x)},
  });
  return k;
}

}  // namespace

HRESULT create_queue(const void* desc, size_t desc_size, const GUID* riid, void** out, const char* how) {
  if (!out) return E_POINTER;
  const auto* d = static_cast<const uint32_t*>(desc);
  if (d) {
    logf("d3d12x: %s(desc %08x %08x %08x %08x %08x %08x, iid %s)", how, d[0], d[1], d[2], d[3],
         desc_size > 16 ? d[4] : 0, desc_size > 16 ? d[5] : 0, riid ? guid_text(*riid).c_str() : "null");
  }
  *out = make_object<Queue>(queue_kind(), desc, d ? desc_size : 0);
  return S_OK;
}

HRESULT create_fence(uint64_t initial, uint32_t flags, const GUID* riid, void** out) {
  if (!out) return E_POINTER;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8) {
    logf("d3d12x: CreateFence(initial %llu, flags %u, iid %s)", static_cast<unsigned long long>(initial), flags,
         riid ? guid_text(*riid).c_str() : "null");
  }
  *out = make_object<Fence>(fence_kind(), initial);
  return S_OK;
}

}  // namespace am::rt::d3d12x
