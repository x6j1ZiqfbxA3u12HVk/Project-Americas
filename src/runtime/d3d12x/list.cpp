// Command allocators and command lists. Lists record the commands the title gives them
// (commands.h); the null back end runs nothing, and each frame's commands are summed up in the
// log.
//
// The console's command list is partly filled by the title's own code, which writes records
// into the list object's fields at fixed offsets. The object therefore reserves a block of
// console-layout fields after its vtable, zeroed: with the record buffer's cursor and end both
// 0, the title's code calls a method for every command.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "runtime/d3d12x/commands.h"
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
  Recording rec;
};

const Kind& list_kind();

// The title's pointers may be bad: a fault leaves the bytes zero.
bool guarded_copy(void* to, const void* from, size_t size) {
  __try {
    std::memcpy(to, from, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void keep(Recording& r, Command& c, uint64_t pointer, uint64_t size) {
  if (!pointer || !size || size > (1u << 20)) return;
  if (!c.data_size) c.data_at = static_cast<uint32_t>(r.data.size());
  const size_t at = r.data.size();
  r.data.resize(at + size);
  if (!guarded_copy(r.data.data() + at, reinterpret_cast<const void*>(pointer), size)) std::memset(r.data.data() + at, 0, size);
  c.data_size += static_cast<uint32_t>(size);
}

uint64_t count_of(uint64_t n) { return std::min<uint64_t>(n, 4096); }

// The bytes behind a command's pointer arguments.
void capture(Recording& r, Command& c) {
  const uint64_t* a = c.a;
  switch (c.op) {
    case kSetViewports: keep(r, c, a[1], count_of(a[0]) * sizeof(D3D12_VIEWPORT)); break;
    case kSetScissorRects: keep(r, c, a[1], count_of(a[0]) * sizeof(D3D12_RECT)); break;
    case kSetBlendFactor: keep(r, c, a[0], 4 * sizeof(float)); break;
    case kSetDescriptorHeaps: keep(r, c, a[1], count_of(a[0]) * sizeof(void*)); break;
    case kSetComputeRoot32BitConstants:
    case kSetGraphicsRoot32BitConstants: keep(r, c, a[2], count_of(a[1]) * sizeof(uint32_t)); break;
    case kSetIndexBuffer: keep(r, c, a[0], sizeof(D3D12_INDEX_BUFFER_VIEW)); break;
    case kSetVertexBuffers: keep(r, c, a[2], count_of(a[1]) * sizeof(D3D12_VERTEX_BUFFER_VIEW)); break;
    case kSetRenderTargets:  // handles (one when they form a range), then the depth handle
      keep(r, c, a[1], (a[2] & 0xFF ? 1 : count_of(a[0])) * sizeof(uint64_t));
      keep(r, c, a[3], sizeof(uint64_t));
      break;
    case kClearRenderTargetView:
      keep(r, c, a[1], 4 * sizeof(float));
      keep(r, c, a[3], count_of(a[2] & 0xFFFFFFFF) * sizeof(D3D12_RECT));
      break;
    case kClearUavUint:
    case kClearUavFloat:
      keep(r, c, a[3], 4 * sizeof(uint32_t));
      keep(r, c, a[5], count_of(a[4] & 0xFFFFFFFF) * sizeof(D3D12_RECT));
      break;
    case kCopyTextureRegion:
      keep(r, c, a[0], sizeof(D3D12_TEXTURE_COPY_LOCATION));
      keep(r, c, a[4], sizeof(D3D12_TEXTURE_COPY_LOCATION));
      keep(r, c, a[5], sizeof(D3D12_BOX));
      break;
    default: break;
  }
}

template <int S>
uint64_t WINAPI record(CommandList* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                       uint64_t a6, uint64_t a7) {
  Command c;
  c.op = static_cast<uint16_t>(S);
  c.a[0] = a1, c.a[1] = a2, c.a[2] = a3, c.a[3] = a4, c.a[4] = a5, c.a[5] = a6, c.a[6] = a7;
  capture(self->rec, c);
  self->rec.commands.push_back(c);
  return 0;
}

// Slot 51: ClearDepthStencilView(handle, flags, depth, stencil, rect count, rects); the depth
// arrives in a float register.
void WINAPI clear_depth_stencil(CommandList* self, uint64_t handle, UINT flags, FLOAT depth, UINT8 stencil, UINT rects,
                                const D3D12_RECT* rect) {
  Command c;
  c.op = kClearDepthStencilView;
  c.a[0] = handle, c.a[1] = flags, c.a[3] = stencil, c.a[4] = rects;
  c.f[0] = depth;
  keep(self->rec, c, reinterpret_cast<uint64_t>(rect), count_of(rects) * sizeof(D3D12_RECT));
  self->rec.commands.push_back(c);
}

HRESULT WINAPI list_close(CommandList* self) {
  self->rec.closed = true;
  return S_OK;
}

HRESULT WINAPI list_reset(CommandList* self, Object* allocator, Object* /*pipeline*/) {
  self->rec.commands.clear();
  self->rec.data.clear();
  self->rec.closed = false;
  if (allocator) self->allocator = allocator;
  return S_OK;
}

template <int S>
Method rec_method() {
  return {S, op_name(S), reinterpret_cast<void*>(&record<S>)};
}

std::vector<Method> list_methods() {
  std::vector<Method> m = {
      {kClose, "Close", reinterpret_cast<void*>(&list_close)},
      {kReset, "Reset", reinterpret_cast<void*>(&list_reset)},
      {kClearDepthStencilView, "ClearDepthStencilView", reinterpret_cast<void*>(&clear_depth_stencil)},
      rec_method<12>(), rec_method<13>(), rec_method<14>(), rec_method<15>(), rec_method<16>(), rec_method<17>(),
      rec_method<18>(), rec_method<19>(), rec_method<20>(), rec_method<21>(), rec_method<22>(), rec_method<23>(),
      rec_method<24>(), rec_method<25>(), rec_method<32>(), rec_method<33>(), rec_method<34>(), rec_method<35>(),
      rec_method<36>(), rec_method<37>(), rec_method<38>(), rec_method<39>(), rec_method<40>(), rec_method<41>(),
      rec_method<42>(), rec_method<43>(), rec_method<44>(), rec_method<45>(), rec_method<46>(), rec_method<47>(),
      rec_method<48>(), rec_method<49>(), rec_method<50>(), rec_method<52>(), rec_method<53>(), rec_method<54>(),
      rec_method<55>(), rec_method<56>(), rec_method<57>(), rec_method<58>(), rec_method<59>(), rec_method<60>(),
      rec_method<61>(), rec_method<62>(), rec_method<63>(), rec_method<146>(),
  };
  return m;
}

const Kind& list_kind() {
  static const Kind k("command list", kMaxSlots, list_methods());
  return k;
}

// ---- the frame's summary ----

struct FrameLog {
  std::mutex lock;
  uint64_t frame = 0;
  uint32_t lists = 0, commands = 0;
  uint32_t counts[kMaxSlots] = {};
  std::string clears;     // the first frames' render-target clears, described
  std::string pipelines;  // and their pipelines and draws, in order
};

FrameLog& frame_log() {
  static FrameLog* const f = new FrameLog;
  return *f;
}

bool summarized(uint64_t frame) { return frame <= 3 || frame % 300 == 0; }

std::string describe_clear(const Recording& r, const Command& c) {
  float color[4] = {};
  if (c.data_size >= sizeof color) std::memcpy(color, r.data.data() + c.data_at, sizeof color);
  char text[256];
  ViewInfo v;
  D3D12_RESOURCE_DESC d{};
  if (read_view(c.a[0], v) && resource_memory(reinterpret_cast<const void*>(v.resource), &d, nullptr)) {
    std::snprintf(text, sizeof text, " [%llux%u format %u: %.3g %.3g %.3g %.3g]", static_cast<unsigned long long>(d.Width),
                  d.Height, v.format ? v.format : d.Format, color[0], color[1], color[2], color[3]);
  } else {
    std::snprintf(text, sizeof text, " [descriptor 0x%llx: %.3g %.3g %.3g %.3g]", static_cast<unsigned long long>(c.a[0]),
                  color[0], color[1], color[2], color[3]);
  }
  return text;
}

}  // namespace

const char* op_name(uint16_t op) {
  switch (op) {
    case kClose: return "Close";
    case kDrawInstanced: return "DrawInstanced";
    case kDrawIndexedInstanced: return "DrawIndexedInstanced";
    case kDispatch: return "Dispatch";
    case kCopyBufferRegion: return "CopyBufferRegion";
    case kCopyTextureRegion: return "CopyTextureRegion";
    case kCopyResource: return "CopyResource";
    case kCopyTiles: return "CopyTiles";
    case kResolveSubresource: return "ResolveSubresource";
    case kSetPrimitiveTopology: return "IASetPrimitiveTopology";
    case kSetViewports: return "RSSetViewports";
    case kSetScissorRects: return "RSSetScissorRects";
    case kSetBlendFactor: return "OMSetBlendFactor";
    case kSetStencilRef: return "OMSetStencilRef";
    case kSetPipelineState: return "SetPipelineState";
    case kSetDescriptorHeaps: return "SetDescriptorHeaps";
    case kSetComputeRootSignature: return "SetComputeRootSignature";
    case kSetGraphicsRootSignature: return "SetGraphicsRootSignature";
    case kSetComputeRootDescriptorTable: return "SetComputeRootDescriptorTable";
    case kSetGraphicsRootDescriptorTable: return "SetGraphicsRootDescriptorTable";
    case kSetComputeRoot32BitConstant: return "SetComputeRoot32BitConstant";
    case kSetGraphicsRoot32BitConstant: return "SetGraphicsRoot32BitConstant";
    case kSetComputeRoot32BitConstants: return "SetComputeRoot32BitConstants";
    case kSetGraphicsRoot32BitConstants: return "SetGraphicsRoot32BitConstants";
    case kSetComputeRootCbv: return "SetComputeRootConstantBufferView";
    case kSetGraphicsRootCbv: return "SetGraphicsRootConstantBufferView";
    case kSetComputeRootSrv: return "SetComputeRootShaderResourceView";
    case kSetGraphicsRootSrv: return "SetGraphicsRootShaderResourceView";
    case kSetComputeRootUav: return "SetComputeRootUnorderedAccessView";
    case kSetGraphicsRootUav: return "SetGraphicsRootUnorderedAccessView";
    case kSetIndexBuffer: return "IASetIndexBuffer";
    case kSetVertexBuffers: return "IASetVertexBuffers";
    case kSetStreamOutputTargets: return "SOSetTargets";
    case kSetRenderTargets: return "OMSetRenderTargets";
    case kClearDepthStencilView: return "ClearDepthStencilView";
    case kClearRenderTargetView: return "ClearRenderTargetView";
    case kClearUavUint: return "ClearUnorderedAccessViewUint";
    case kClearUavFloat: return "ClearUnorderedAccessViewFloat";
    case kDiscardResource: return "DiscardResource";
    case kBeginQuery: return "BeginQuery";
    case kEndQuery: return "EndQuery";
    case kResolveQueryData: return "ResolveQueryData";
    case kSetPredication: return "SetPredication";
    case kSetMarker: return "SetMarker";
    case kBeginEvent: return "BeginEvent";
    case kEndEvent: return "EndEvent";
    case kExecuteIndirect: return "ExecuteIndirect";
    case kReset: return "Reset";
    case kBarrier: return "Barrier";
    default: return nullptr;
  }
}

const Recording* recording_of(const void* list) {
  Object* o = object_of_kind(list, list_kind());
  return o ? &static_cast<CommandList*>(o)->rec : nullptr;
}

void frame_execute(const void* list) {
  const Recording* r = recording_of(list);
  if (!r) return;
  FrameLog& f = frame_log();
  std::lock_guard g(f.lock);
  ++f.lists;
  f.commands += static_cast<uint32_t>(r->commands.size());
  const bool describe = summarized(f.frame + 1) && f.clears.size() + f.pipelines.size() < 4000;
  for (const Command& c : r->commands) {
    if (c.op < kMaxSlots) ++f.counts[c.op];
    if (describe && c.op == kClearRenderTargetView) f.clears += describe_clear(*r, c);
    if (describe && c.op == kSetPipelineState) f.pipelines += " [" + describe_pipeline(reinterpret_cast<const void*>(c.a[0])) + "]";
    if (describe && (c.op == kDrawInstanced || c.op == kDrawIndexedInstanced)) {
      f.pipelines += " " + std::string(c.op == kDrawInstanced ? "draw" : "indexed draw") + "(" + std::to_string(c.a[0] & 0xFFFFFFFF) +
                     " x " + std::to_string(c.a[1] & 0xFFFFFFFF) + ")";
    }
  }
}

void frame_present() {
  FrameLog& f = frame_log();
  std::lock_guard g(f.lock);
  ++f.frame;
  if (summarized(f.frame)) {
    std::string ops;
    for (int op = 0; op < kMaxSlots; ++op) {
      if (!f.counts[op]) continue;
      const char* name = op_name(static_cast<uint16_t>(op));
      ops += (ops.empty() ? "" : ", ") + (name ? std::string(name) : "slot " + std::to_string(op)) + " " +
             std::to_string(f.counts[op]);
    }
    logf("frame %llu: %u lists, %u commands: %s", static_cast<unsigned long long>(f.frame), f.lists, f.commands,
         ops.c_str());
    if (!f.clears.empty()) logf("frame %llu: render-target clears:%s", static_cast<unsigned long long>(f.frame), f.clears.c_str());
    if (!f.pipelines.empty()) logf("frame %llu: pipelines and draws:%s", static_cast<unsigned long long>(f.frame), f.pipelines.c_str());
  }
  f.lists = f.commands = 0;
  std::memset(f.counts, 0, sizeof f.counts);
  f.clears.clear();
  f.pipelines.clear();
}

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
