// Command allocators and command lists. Each console command becomes, as the title makes it,
// the host D3D12 call that does the same, in a host command list of the console list's own
// (no recording and replaying): clears, render targets, viewports and the like so far. Draws and
// dispatches wait for their pipelines (shaders compiled ahead of time). Lists also keep what
// they were given (commands.h), which the log sums up per frame.
//
// The console's command list is partly filled by the title's own code, which writes records
// into the list object's fields at fixed offsets. The object therefore reserves a block of
// console-layout fields after its vtable, zeroed: with the record buffer's cursor and end both
// 0, the title's code calls a method for every command.

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "core/text.h"
#include "runtime/d3d12x/bindings.h"
#include "runtime/d3d12x/commands.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/gpu_memory.h"
#include "runtime/d3d12x/host.h"
#include "runtime/hle/xmem.h"
#include "runtime/d3d12x/root_signature.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"
#include "runtime/script.h"
#include "runtime/present/host_gpu.h"

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

using Microsoft::WRL::ComPtr;

// The host command list of a console list. It opens on the first command after a Reset, keeps
// each resource's state within itself (every resource enters and leaves the list in COMMON, so
// lists can be submitted in any order) and closes with the console list.
struct HostList {
  struct Allocator {
    ComPtr<ID3D12CommandAllocator> allocator;
    uint64_t fence = 0;  // reusable once the host fence reaches this; UINT64_MAX while recording
  };
  ComPtr<ID3D12GraphicsCommandList> list;
  std::vector<Allocator> allocators;
  size_t current = 0;
  bool open = false, ready = false;  // recording; closed and waiting to be submitted
  std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> states;
  // Graphics state for draws: the console root signature and pipeline, each root parameter's
  // argument (an address) or constants.
  static constexpr size_t kParams = 64;
  const void* root_signature = nullptr;
  const void* pipeline = nullptr;
  uint64_t args[kParams] = {};
  std::vector<uint32_t> constants[kParams];
  bool bound = false;  // the translated root signature and heap are set in the host list
  bool heaps_set = false;  // the shader and sampler heaps, the page table and bindings flushed
  bool compute_bound = false;  // the translated compute root signature and table are set
  // Compute state, kept apart from graphics as D3D12 does.
  const void* compute_root_signature = nullptr;
  uint64_t compute_args[kParams] = {};
  std::vector<uint32_t> compute_constants[kParams];
  bool flushed = false;  // it holds the image bindings' upkeep (and their misses' readback)
  std::vector<std::string> dispatched;  // the compute programs it runs (markers), for slow-GPU reports
  DXGI_FORMAT targets[8] = {};  // the bound render targets' view formats
  uint32_t target_count = 0;
  // What this host list has been given (D3D12 lists inherit nothing from each other).
  bool has_viewport = false, has_scissor = false, has_topology = false, has_depth = false;
  uint32_t topology = 0;
  DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;  // the bound depth view's format
  bool rect_list = false;  // the console's rect list topology is set
  D3D12_VIEWPORT viewport{};
};

// Space a list hands out for data its commands carry (slot 148): title memory from the graphics
// allocator, which the GPU reads as it reads the rest of the title's memory. Chunks stay with the
// list and are handed out again from the start when the list is reset.
struct ListSpace {
  std::vector<std::pair<uint8_t*, uint64_t>> chunks;
  size_t chunk = 0;
  uint64_t used = 0;
  size_t space_count() const { return chunks.size(); }
};

struct CommandList : Object {
  uint8_t console[kConsoleFields - sizeof(void*)] = {};  // the console object's fields, as the title sees them
  uint32_t desc[4] = {};
  Object* allocator = nullptr;
  Recording rec;
  HostList host;
  ListSpace space;
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

// ---- the host list ----

void forget_dispatch_names(const void* list);
std::string dispatch_name(const void* list, uint32_t nth);

ID3D12GraphicsCommandList* host_begin(CommandList* l) {
  HostList& h = l->host;
  if (h.open) return h.list.Get();
  present::HostGpu* gpu = present::host_gpu();
  if (!gpu) return nullptr;
  size_t pick = h.allocators.size();
  for (size_t i = 0; i < h.allocators.size(); ++i) {
    if (gpu->done(h.allocators[i].fence)) {
      pick = i;
      break;
    }
  }
  if (pick == h.allocators.size()) {
    HostList::Allocator a;
    if (FAILED(gpu->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.allocator)))) return nullptr;
    h.allocators.push_back(std::move(a));
  }
  ID3D12CommandAllocator* a = h.allocators[pick].allocator.Get();
  a->Reset();
  if (!h.list) {
    if (FAILED(gpu->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&h.list)))) return nullptr;
  } else if (FAILED(h.list->Reset(a, nullptr))) {
    return nullptr;
  }
  h.allocators[pick].fence = UINT64_MAX;
  h.current = pick;
  h.open = true;
  h.dispatched.clear();
  if (present::dred_enabled()) {
    forget_dispatch_names(h.list.Get());
    static std::once_flag once;
    std::call_once(once, [] { present::set_dispatch_namer(&dispatch_name); });
  }
  h.ready = false;
  h.states.clear();
  h.bound = false;
  h.heaps_set = false;
  h.compute_bound = false;
  h.flushed = false;
  h.has_viewport = h.has_scissor = h.has_topology = false;
  return h.list.Get();
}

// Moves `r` into `state` within the list.
void host_use(HostList& h, ID3D12Resource* r, D3D12_RESOURCE_STATES state) {
  for (auto& [res, s] : h.states) {
    if (res != r) continue;
    if (s != state) {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, s, state};
      h.list->ResourceBarrier(1, &b);
      s = state;
    }
    return;
  }
  if (state != D3D12_RESOURCE_STATE_COMMON) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, state};
    h.list->ResourceBarrier(1, &b);
  }
  h.states.emplace_back(r, state);
}

void host_close(HostList& h) {
  if (!h.open) return;
  std::vector<D3D12_RESOURCE_BARRIER> back;
  for (auto& [r, s] : h.states) {
    if (s == D3D12_RESOURCE_STATE_COMMON) continue;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, s, D3D12_RESOURCE_STATE_COMMON};
    back.push_back(b);
  }
  if (!back.empty()) h.list->ResourceBarrier(static_cast<UINT>(back.size()), back.data());
  h.states.clear();
  const HRESULT hr = h.list->Close();
  const bool ok = SUCCEEDED(hr);
  h.open = false;
  h.ready = ok;
  if (!ok) h.allocators[h.current].fence = 0;
  // A list the host refuses is dropped whole, its draws with it: say so.
  static std::atomic<uint64_t> closed{0}, refused{0};
  ++closed;
  if (!ok && refused.fetch_add(1, std::memory_order_relaxed) < 8) {
    logf("gpu: the host refused a command list at Close (0x%08lx); %llu of %llu so far", static_cast<unsigned long>(hr),
         static_cast<unsigned long long>(refused.load()), static_cast<unsigned long long>(closed.load()));
  }
}

// A console render-target (or depth) descriptor as a host view, the texture moved into `state`.
D3D12_CPU_DESCRIPTOR_HANDLE host_view(HostList& h, uint64_t descriptor, bool depth) {
  ViewInfo v;
  if (!read_view(descriptor, v) || v.kind != (depth ? 5 : 4)) return {};
  ID3D12Resource* t = host_texture(reinterpret_cast<const void*>(v.resource));
  if (!t) return {};
  const D3D12_CPU_DESCRIPTOR_HANDLE view = depth ? host_dsv(t, v.format) : host_rtv(t, v.format);
  if (view.ptr) host_use(h, t, depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
  return view;
}

const uint8_t* data_of(const Recording& r, const Command& c) { return c.data_size ? r.data.data() + c.data_at : nullptr; }

// Root arguments and pipelines are state the draws read; they need no host list yet.
bool track(HostList& h, const Recording& r, const Command& c) {
  const size_t index = static_cast<size_t>(c.a[0]);
  switch (c.op) {
    case kSetGraphicsRootSignature:
      h.root_signature = reinterpret_cast<const void*>(c.a[0]);
      for (auto& v : h.constants) v.clear();
      std::fill(std::begin(h.args), std::end(h.args), 0);
      return true;
    case kSetPipelineState: h.pipeline = reinterpret_cast<const void*>(c.a[0]); return true;
    case kSetGraphicsRootDescriptorTable:
    case kSetGraphicsRootCbv:
    case kSetGraphicsRootSrv:
    case kSetGraphicsRootUav:
      if (index < HostList::kParams) h.args[index] = c.a[1];
      return true;
    case kSetGraphicsRoot32BitConstant:
      if (index < HostList::kParams) {
        auto& v = h.constants[index];
        const size_t at = static_cast<size_t>(c.a[2] & 0xFFFF);
        if (v.size() <= at) v.resize(at + 1, 0);
        v[at] = static_cast<uint32_t>(c.a[1]);
      }
      return true;
    case kSetGraphicsRoot32BitConstants:
      if (index < HostList::kParams && c.data_size) {
        auto& v = h.constants[index];
        const size_t n = c.data_size / 4, at = static_cast<size_t>(c.a[3] & 0xFFFF);
        if (v.size() < at + n) v.resize(at + n, 0);
        std::memcpy(v.data() + at, r.data.data() + c.data_at, n * 4);
      }
      return true;
    case kSetComputeRootSignature:
      h.compute_root_signature = reinterpret_cast<const void*>(c.a[0]);
      for (auto& v : h.compute_constants) v.clear();
      std::fill(std::begin(h.compute_args), std::end(h.compute_args), 0);
      return true;
    case kSetComputeRootDescriptorTable:
    case kSetComputeRootCbv:
    case kSetComputeRootSrv:
    case kSetComputeRootUav:
      if (index < HostList::kParams) h.compute_args[index] = c.a[1];
      return true;
    case kSetComputeRoot32BitConstant:
      if (index < HostList::kParams) {
        auto& v = h.compute_constants[index];
        const size_t at = static_cast<size_t>(c.a[2] & 0xFFFF);
        if (v.size() <= at) v.resize(at + 1, 0);
        v[at] = static_cast<uint32_t>(c.a[1]);
      }
      return true;
    case kSetComputeRoot32BitConstants:
      if (index < HostList::kParams && c.data_size) {
        auto& v = h.compute_constants[index];
        const size_t n = c.data_size / 4, at = static_cast<size_t>(c.a[3] & 0xFFFF);
        if (v.size() < at + n) v.resize(at + n, 0);
        std::memcpy(v.data() + at, r.data.data() + c.data_at, n * 4);
      }
      return true;
    default: return false;
  }
}

std::atomic<uint64_t> g_draws_run{0}, g_draws_skipped{0}, g_dispatches_run{0}, g_dispatches_skipped{0};
constexpr uint64_t kRectListTopology = 17;

void note_unreachable_buffer() {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) logf("gpu: a vertex or index buffer outside GPU-reachable title memory (bound null)");
}

// Once a list: the page table's and the bindings' upkeep, then the shader and sampler heaps.
// Returns the binding table's address (0: none, so nothing can run).
D3D12_GPU_VIRTUAL_ADDRESS set_heaps(HostList& h, ID3D12GraphicsCommandList* list) {
  if (!h.heaps_set) {
    flush_page_table(list);
    flush_bindings(list);  // builds the table first: it is never bound null
    h.flushed = true;
    ID3D12DescriptorHeap* heaps[] = {translated_shader_heap(), sampler_heap()};
    if (!heaps[0] || !heaps[1]) return 0;
    list->SetDescriptorHeaps(2, heaps);
    h.heaps_set = true;
  }
  return bindings_table();
}

// With DRED: each host list's dispatches, in order (program and grid), to name the one a lost
// device stopped at. Cleared when the list is reset.
std::mutex g_dispatch_names_lock;
std::map<const void*, std::vector<std::string>> g_dispatch_names;

void note_dispatch_name(const void* list, std::string name) {
  std::lock_guard g(g_dispatch_names_lock);
  auto& v = g_dispatch_names[list];
  if (v.size() < 16384) v.push_back(std::move(name));
}

void forget_dispatch_names(const void* list) {
  std::lock_guard g(g_dispatch_names_lock);
  g_dispatch_names.erase(list);
}

std::string dispatch_name(const void* list, uint32_t nth) {
  std::lock_guard g(g_dispatch_names_lock);
  auto it = g_dispatch_names.find(list);
  if (it == g_dispatch_names.end() || nth >= it->second.size()) return {};
  return "program " + it->second[nth];
}

// A dispatch, when its compute pipeline has a host pipeline: the translated compute root
// signature with the user data and the binding table, the dispatch, then a barrier so what it
// wrote is seen by the work after it.
void dispatch(HostList& h, ID3D12GraphicsCommandList* list, const Command& c) {
  // AMERICAS_NO_COMPUTE=1 skips dispatches (to tell compute's part in a picture apart).
  static const bool no_compute = GetEnvironmentVariableA("AMERICAS_NO_COMPUTE", nullptr, 0) != 0;
  if (no_compute) {
    ++g_dispatches_skipped;
    return;
  }
  const void* rs_object = nullptr;
  // AMERICAS_PIPELINE_WAIT_MS: how long dispatches of one pipeline wait for it in all (5 s).
  static const uint32_t wait_ms = [] {
    char text[16] = {};
    return GetEnvironmentVariableA("AMERICAS_PIPELINE_WAIT_MS", text, sizeof text) ? static_cast<uint32_t>(std::strtoul(text, nullptr, 10))
                                                                                     : 5000u;
  }();
  ID3D12PipelineState* pso = host_compute_pipeline_waiting(h.pipeline, &rs_object, wait_ms);
  const RootSignatureDesc* rs = root_signature_desc(h.compute_root_signature ? h.compute_root_signature : rs_object);
  if (!pso || !rs) {
    ++g_dispatches_skipped;
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 12) {
      logf("gpu: a dispatch waits: %s (%s)", describe_pipeline(h.pipeline).c_str(),
           !pso ? "no host pipeline yet" : "no root signature");
    }
    return;
  }
  const D3D12_GPU_VIRTUAL_ADDRESS table = set_heaps(h, list);
  if (!table) {
    ++g_dispatches_skipped;
    return;
  }
  if (h.dispatched.size() < 64) {
    std::string m = compute_marker_of(h.pipeline);
    if (std::find(h.dispatched.begin(), h.dispatched.end(), m) == h.dispatched.end()) h.dispatched.push_back(std::move(m));
  }
  if (!h.compute_bound) {
    list->SetComputeRootSignature(translated_root_signature());
    list->SetComputeRootUnorderedAccessView(1, table);
    h.compute_bound = true;
  }
  uint32_t cs[kUserDataConstants] = {};
  if (const std::vector<UserSlot>* layout = compute_user_layout(h.pipeline)) {
    // The program's own placement: each root parameter at the user-data dword its metadata names.
    for (const UserSlot& u : *layout) {
      if (u.slot >= HostList::kParams || u.user >= kUserDataConstants) continue;
      if (u.root == 3) {
        cs[u.user] = static_cast<uint32_t>(h.compute_args[u.slot]);
      } else if (u.root == 1) {
        cs[u.user] = static_cast<uint32_t>(h.compute_args[u.slot]);
        if (u.user + 1 < kUserDataConstants) cs[u.user + 1] = static_cast<uint32_t>(h.compute_args[u.slot] >> 32);
      } else if (u.root == 0) {
        const std::vector<uint32_t>& values = h.compute_constants[u.slot];
        for (uint32_t k = 0; k < values.size() && u.user + k < kUserDataConstants; ++k) cs[u.user + k] = values[k];
      }
    }
  } else {
    user_data_sgprs(*rs, D3D12_SHADER_VISIBILITY_ALL, h.compute_args, h.compute_constants, cs, kUserDataConstants);
  }
  {
    static std::mutex told_lock;
    static std::set<const void*> told;
    std::lock_guard g(told_lock);
    if (told.size() < 6 && told.insert(rs).second) {
      std::string params;
      for (const RootParameter& p : rs->parameters) {
        params += " [type " + std::to_string(p.type) + " vis " + std::to_string(p.visibility) + " n " + std::to_string(p.constants) + "]";
      }
      logf("gpu: compute root signature of %s:%s", describe_pipeline(h.pipeline).c_str(), params.c_str());
    }
  }
  // Large dispatches are named (the GPU ran some for seconds, and lost the device): D3D12 takes
  // at most 65,535 groups a dimension, the console more; those are skipped, not sent.
  const uint64_t gx = static_cast<uint32_t>(c.a[0]), gy = static_cast<uint32_t>(c.a[1]), gz = static_cast<uint32_t>(c.a[2]);
  const bool too_large = gx > 65535 || gy > 65535 || gz > 65535;
  if (too_large || gx * gy * gz > (uint64_t{1} << 20)) {
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 24) {
      logf("gpu: a dispatch of %llu x %llu x %llu groups (%s)%s", static_cast<unsigned long long>(gx), static_cast<unsigned long long>(gy),
           static_cast<unsigned long long>(gz), compute_marker_of(h.pipeline).substr(0, 16).c_str(),
           too_large ? ": over D3D12's 65,535 a dimension, skipped" : "");
    }
    if (too_large) {
      ++g_dispatches_skipped;
      return;
    }
  }
  if (gx == 0 || gy == 0 || gz == 0) return;  // nothing to run
  if (present::dred_enabled()) {  // names the dispatch's program and grid in DRED's breadcrumbs
    note_dispatch_name(list, compute_marker_of(h.pipeline).substr(0, 16) + " " + std::to_string(gx) + "x" + std::to_string(gy) + "x" +
                                 std::to_string(gz));
    char text[96];
    std::snprintf(text, sizeof text, "CS %s %llux%llux%llu", compute_marker_of(h.pipeline).substr(0, 16).c_str(),
                  static_cast<unsigned long long>(gx), static_cast<unsigned long long>(gy), static_cast<unsigned long long>(gz));
    const std::string d = text;
    const std::wstring w(d.begin(), d.end());
    list->SetMarker(0, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
  }
  list->SetPipelineState(pso);
  list->SetComputeRoot32BitConstants(0, kUserDataConstants, cs, 0);
  list->Dispatch(static_cast<UINT>(gx), static_cast<UINT>(gy), static_cast<UINT>(gz));
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  list->ResourceBarrier(1, &b);
  ++g_dispatches_run;
}

// A draw, when its pipeline has a host pipeline: the translated root signature and heaps, the
// image binding table, each stage's user data, the draw.
void draw(HostList& h, ID3D12GraphicsCommandList* list, const Command& c) {
  const void* rs_object = nullptr;
  ID3D12PipelineState* pso = host_pipeline(h.pipeline, &rs_object, h.targets, h.target_count, h.depth_format, h.rect_list);
  const RootSignatureDesc* rs = root_signature_desc(h.root_signature ? h.root_signature : rs_object);
  if (!pso || !rs) {
    ++g_draws_skipped;
    return;
  }
  if (!h.bound) {
    const D3D12_GPU_VIRTUAL_ADDRESS table = set_heaps(h, list);
    if (!table) {
      ++g_draws_skipped;
      return;
    }
    list->SetGraphicsRootSignature(translated_graphics_root_signature());
    list->SetGraphicsRootUnorderedAccessView(2, table);
    h.bound = true;
  }
  uint32_t vs[kUserDataConstants] = {}, ps[kUserDataConstants] = {};
  user_data_sgprs(*rs, D3D12_SHADER_VISIBILITY_VERTEX, h.args, h.constants, vs, kUserDataConstants);
  user_data_sgprs(*rs, D3D12_SHADER_VISIBILITY_PIXEL, h.args, h.constants, ps, kUserDataConstants);
  list->SetPipelineState(pso);
  list->SetGraphicsRoot32BitConstants(0, kUserDataConstants, vs, 0);
  list->SetGraphicsRoot32BitConstants(1, kUserDataConstants, ps, 0);
  if (present::dred_enabled()) {  // names the draw in DRED's breadcrumbs
    const std::string d = describe_pipeline(h.pipeline);
    const std::wstring w(d.begin(), d.end());
    list->SetMarker(0, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
  }
  script_on_draw(h.pipeline, [&] { return describe_pipeline(h.pipeline); }, c.a[0] | (c.a[1] << 32));  // a scripted step may wait for it
  if (c.op == kDrawIndexedInstanced) {
    list->DrawIndexedInstanced(static_cast<UINT>(c.a[0]), static_cast<UINT>(c.a[1]), static_cast<UINT>(c.a[2]),
                               static_cast<INT>(c.a[3]), static_cast<UINT>(c.a[4]));
  } else {
    list->DrawInstanced(static_cast<UINT>(c.a[0]), static_cast<UINT>(c.a[1]), static_cast<UINT>(c.a[2]), static_cast<UINT>(c.a[3]));
  }
  ++g_draws_run;
}

// The host call for a command, made as the title makes the console one.
void forward(CommandList* l, const Command& c) {
  if (track(l->host, l->rec, c)) return;
  switch (c.op) {
    case kDrawInstanced:
    case kDrawIndexedInstanced:
    case kDispatch:
    case kClearRenderTargetView:
    case kClearDepthStencilView:
    case kSetRenderTargets:
    case kSetViewports:
    case kSetScissorRects:
    case kSetPrimitiveTopology:
    case kSetBlendFactor:
    case kSetStencilRef:
    case kSetVertexBuffers:
    case kSetIndexBuffer: break;
    default: return;  // the rest (copies, queries, ...) is not run on the host yet
  }
  ID3D12GraphicsCommandList* list = host_begin(l);
  if (!list) return;
  HostList& h = l->host;
  const Recording& r = l->rec;
  const uint8_t* data = data_of(r, c);
  switch (c.op) {
    case kClearRenderTargetView: {
      const D3D12_CPU_DESCRIPTOR_HANDLE view = host_view(h, c.a[0], false);
      if (!view.ptr || c.data_size < 16) return;
      const UINT rects = static_cast<UINT>(c.a[2]);
      static const bool debug_clear = [] {
        char* v = nullptr;
        size_t n = 0;
        const bool on = _dupenv_s(&v, &n, "AMERICAS_DEBUG_PS") == 0 && v;
        std::free(v);
        return on;
      }();
      static const float kDebugColor[4] = {0.0f, 0.0f, 0.4f, 1.0f};  // AMERICAS_DEBUG_PS: clears show dark blue
      if (debug_clear && GetEnvironmentVariableA("AMERICAS_DEBUG_NOCLEAR", nullptr, 0)) return;
      list->ClearRenderTargetView(view, debug_clear ? kDebugColor : reinterpret_cast<const float*>(data),
                                  rects && c.data_size >= 16 + rects * 16 ? rects : 0,
                                  rects ? reinterpret_cast<const D3D12_RECT*>(data + 16) : nullptr);
      return;
    }
    case kClearDepthStencilView: {
      const D3D12_CPU_DESCRIPTOR_HANDLE view = host_view(h, c.a[0], true);
      if (!view.ptr) return;
      const UINT rects = data ? static_cast<UINT>(c.a[4]) : 0;
      list->ClearDepthStencilView(view, static_cast<D3D12_CLEAR_FLAGS>(c.a[1]), c.f[0], static_cast<UINT8>(c.a[3]), rects,
                                  rects ? reinterpret_cast<const D3D12_RECT*>(data) : nullptr);
      return;
    }
    case kSetRenderTargets: {
      const UINT count = std::min<UINT>(static_cast<UINT>(c.a[0]), D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
      const bool range = (c.a[2] & 0xFF) != 0;
      const uint64_t* handles = reinterpret_cast<const uint64_t*>(data);
      const size_t given = (range ? (count ? 1 : 0) : count);
      if (given * 8 > c.data_size) return;
      // The targets drawn so far may be read by the next draws (bindless: we cannot see which;
      // the UI draws a clip mask, then samples it at once). Their writes are flushed here: each
      // goes back to COMMON, from which reads promote; those bound again return just below.
      {
        std::vector<D3D12_RESOURCE_BARRIER> back;
        for (auto& [target, st] : h.states) {
          if (st != D3D12_RESOURCE_STATE_RENDER_TARGET && st != D3D12_RESOURCE_STATE_DEPTH_WRITE) continue;
          D3D12_RESOURCE_BARRIER b{};
          b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          b.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, D3D12_RESOURCE_STATE_COMMON};
          back.push_back(b);
          st = D3D12_RESOURCE_STATE_COMMON;
        }
        if (!back.empty()) list->ResourceBarrier(static_cast<UINT>(back.size()), back.data());
        // Targets read in another format: their copies are brought up to date.
        for (const D3D12_RESOURCE_BARRIER& done : back) {
          ID3D12Resource* target = done.Transition.pResource;
          if (!target_has_copies(target)) continue;
          host_use(h, target, D3D12_RESOURCE_STATE_COPY_SOURCE);
          copy_target(list, target);
        }
      }
      D3D12_CPU_DESCRIPTOR_HANDLE views[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
      h.target_count = 0;
      for (UINT i = 0; i < count; ++i) {
        const uint64_t console = range ? handles[0] + uint64_t{i} * descriptor_size(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : handles[i];
        views[i] = host_view(h, console, false);
        if (!views[i].ptr) return;  // a target we cannot show: the draws after it are skipped anyway
        ViewInfo v;
        h.targets[i] = read_view(console, v) ? host_rtv_format(host_texture(reinterpret_cast<const void*>(v.resource)), v.format)
                                             : DXGI_FORMAT_UNKNOWN;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 12) {
          D3D12_RESOURCE_DESC d{};
          uint64_t at = 0;
          resource_memory(reinterpret_cast<const void*>(v.resource), &d, &at);
          logf("gpu: render target %u: %llux%u format %u (view %u) at 0x%s", i, static_cast<unsigned long long>(d.Width), d.Height,
               d.Format, h.targets[i], hex(at).c_str());
        }
      }
      h.target_count = count;
      D3D12_CPU_DESCRIPTOR_HANDLE depth{};
      h.depth_format = DXGI_FORMAT_UNKNOWN;
      if (c.a[3] && c.data_size >= given * 8 + 8) {
        depth = host_view(h, handles[given], true);
        ViewInfo v;
        if (depth.ptr && read_view(handles[given], v)) {
          h.depth_format = host_dsv_format(host_texture(reinterpret_cast<const void*>(v.resource)), v.format);
        }
      }
      list->OMSetRenderTargets(count, count ? views : nullptr, FALSE, depth.ptr ? &depth : nullptr);
      h.has_depth = depth.ptr != 0;
      return;
    }
    case kSetViewports:
      if (data && c.data_size >= sizeof(D3D12_VIEWPORT)) {
        list->RSSetViewports(static_cast<UINT>(c.data_size / sizeof(D3D12_VIEWPORT)), reinterpret_cast<const D3D12_VIEWPORT*>(data));
        h.has_viewport = true;
        std::memcpy(&h.viewport, data, sizeof h.viewport);
      }
      return;
    case kSetScissorRects:
      if (data) {
        list->RSSetScissorRects(static_cast<UINT>(c.data_size / sizeof(D3D12_RECT)), reinterpret_cast<const D3D12_RECT*>(data));
        h.has_scissor = true;
      }
      return;
    case kSetPrimitiveTopology:
      // The console's rect list (17: three corners of a rectangle) is drawn as triangles, and the
      // pipeline adds the fourth corner (host_pipeline).
      h.rect_list = c.a[0] == kRectListTopology;
      list->IASetPrimitiveTopology(h.rect_list ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : static_cast<D3D12_PRIMITIVE_TOPOLOGY>(c.a[0]));
      h.has_topology = true;
      h.topology = static_cast<uint32_t>(c.a[0]);
      return;
    case kSetBlendFactor: list->OMSetBlendFactor(reinterpret_cast<const float*>(data)); return;
    case kSetStencilRef: list->OMSetStencilRef(static_cast<UINT>(c.a[0])); return;
    case kDrawInstanced:
    case kDrawIndexedInstanced: draw(h, list, c); return;
    case kDispatch: dispatch(h, list, c); return;
    // Vertex and index buffers in place: title memory at its host GPU address (null when the
    // memory cannot be reached that way yet).
    case kSetVertexBuffers: {
      const UINT start = static_cast<UINT>(c.a[0]);
      const UINT count = std::min<UINT>(static_cast<UINT>(c.a[1]), D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
      if (!data || c.data_size < count * sizeof(D3D12_VERTEX_BUFFER_VIEW) || start + count > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) return;
      D3D12_VERTEX_BUFFER_VIEW views[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
      std::memcpy(views, data, count * sizeof(D3D12_VERTEX_BUFFER_VIEW));
      for (UINT i = 0; i < count; ++i) {
        views[i].BufferLocation = host_gpu_address(views[i].BufferLocation, views[i].SizeInBytes);
        if (!views[i].BufferLocation) {
          views[i].SizeInBytes = 0;
          note_unreachable_buffer();
        }
      }
      list->IASetVertexBuffers(start, count, views);
      return;
    }
    case kSetIndexBuffer: {
      if (!data || c.data_size < sizeof(D3D12_INDEX_BUFFER_VIEW)) return;
      D3D12_INDEX_BUFFER_VIEW v;
      std::memcpy(&v, data, sizeof v);
      v.BufferLocation = host_gpu_address(v.BufferLocation, v.SizeInBytes);
      if (!v.BufferLocation) note_unreachable_buffer();
      list->IASetIndexBuffer(v.BufferLocation ? &v : nullptr);
      return;
    }
    default: return;
  }
}

template <int S>
uint64_t WINAPI record(CommandList* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                       uint64_t a6, uint64_t a7) {
  Command c;
  c.op = static_cast<uint16_t>(S);
  c.a[0] = a1, c.a[1] = a2, c.a[2] = a3, c.a[3] = a4, c.a[4] = a5, c.a[5] = a6, c.a[6] = a7;
  if constexpr (S == 75 || S == 126 || S == 131 || S == 132 || S == 142) {
    // Taken quietly; their first calls are logged (one may write a value the title waits for).
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 6) {
      logf("d3d12x: list slot %d (%s): %llx %llx %llx %llx %llx %llx", S, "quiet", static_cast<unsigned long long>(a1),
           static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
           static_cast<unsigned long long>(a5), static_cast<unsigned long long>(a6));
    }
  }
  capture(self->rec, c);
  self->rec.commands.push_back(c);
  forward(self, self->rec.commands.back());
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
  forward(self, self->rec.commands.back());
}

HRESULT WINAPI list_close(CommandList* self) {
  self->rec.closed = true;
  host_close(self->host);
  return S_OK;
}

// Slot 148: space for `size` bytes at `alignment` that the list's commands use (the BVH builder
// writes its build inputs there); nullptr when there is no memory (the caller then throws).
void* WINAPI list_allocate_space(CommandList* self, uint64_t size, uint64_t alignment) {
  constexpr uint64_t kChunk = 1u << 20;
  alignment = std::max<uint64_t>(std::bit_ceil(std::max<uint64_t>(alignment, 16)), 16);
  ListSpace& a = self->space;
  for (;;) {
    if (a.chunk < a.space_count()) {
      auto [base, capacity] = a.chunks[a.chunk];
      const uint64_t at = (a.used + alignment - 1) & ~(alignment - 1);
      if (at + size <= capacity) {
        a.used = at + size;
        return base + at;
      }
      ++a.chunk;
      a.used = 0;
      continue;
    }
    const uint64_t capacity = std::max(kChunk, (size + alignment + 0xFFFF) & ~uint64_t{0xFFFF});
    void* memory = hle::xmem_alloc(capacity, resource_attributes(capacity, 65536));
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 8) {
      logf("d3d12x: command list space: %s more at 0x%s (asked %llu bytes, alignment %llu)", format_bytes(capacity).c_str(),
           hex(reinterpret_cast<uint64_t>(memory)).c_str(), static_cast<unsigned long long>(size),
           static_cast<unsigned long long>(alignment));
    }
    if (!memory) return nullptr;
    a.chunks.emplace_back(static_cast<uint8_t*>(memory), capacity);
  }
}

HRESULT WINAPI list_reset(CommandList* self, Object* allocator, Object* /*pipeline*/) {
  self->space.chunk = 0;
  self->space.used = 0;
  self->rec.commands.clear();
  self->rec.data.clear();
  self->rec.closed = false;
  // A host list never submitted gives its allocator back.
  HostList& h = self->host;
  if (h.open) host_close(h);
  if (h.ready) h.allocators[h.current].fence = 0;
  h.ready = false;
  if (allocator) self->allocator = allocator;
  return S_OK;
}

template <int S>
Method rec_method() {
  return {S, op_name(S), reinterpret_cast<void*>(&record<S>)};
}

// Slot 80: Dispatch(x, y, z, dispatch initiator flags), what the BVH builder calls when its
// list has no room for the dispatch packet it would write itself. Recorded as a Dispatch.
uint64_t WINAPI dispatch_with_initiator(CommandList* self, uint64_t x, uint64_t y, uint64_t z, uint64_t /*initiator*/) {
  return record<kDispatch>(self, x & 0xFFFFFFFF, y & 0xFFFFFFFF, z & 0xFFFFFFFF, 0, 0, 0, 0);
}

std::vector<Method> list_methods() {
  std::vector<Method> m = {
      {kClose, "Close", reinterpret_cast<void*>(&list_close)},
      {kReset, "Reset", reinterpret_cast<void*>(&list_reset)},
      {kClearDepthStencilView, "ClearDepthStencilView", reinterpret_cast<void*>(&clear_depth_stencil)},
      {148, "AllocateSpace", reinterpret_cast<void*>(&list_allocate_space)},
      {80, "DispatchX", reinterpret_cast<void*>(&dispatch_with_initiator)},
      rec_method<12>(), rec_method<13>(), rec_method<14>(), rec_method<15>(), rec_method<16>(), rec_method<17>(),
      rec_method<18>(), rec_method<19>(), rec_method<20>(), rec_method<21>(), rec_method<22>(), rec_method<23>(),
      rec_method<24>(), rec_method<25>(), rec_method<26>(), rec_method<32>(), rec_method<33>(), rec_method<34>(), rec_method<35>(),
      rec_method<36>(), rec_method<37>(), rec_method<38>(), rec_method<39>(), rec_method<40>(), rec_method<41>(),
      rec_method<42>(), rec_method<43>(), rec_method<44>(), rec_method<45>(), rec_method<46>(), rec_method<47>(),
      rec_method<48>(), rec_method<49>(), rec_method<50>(), rec_method<52>(), rec_method<53>(), rec_method<54>(),
      rec_method<55>(), rec_method<56>(), rec_method<57>(), rec_method<58>(), rec_method<59>(), rec_method<60>(),
      rec_method<61>(), rec_method<62>(), rec_method<63>(), rec_method<146>(), rec_method<70>(), rec_method<71>(),
      rec_method<72>(), rec_method<73>(), rec_method<74>(),
      // State the host has no use for yet, taken quietly: 75 (pixel shader state), 126 (shading
      // rate), 131 and 132 (pipeline syncs; host barriers order our work), 142.
      rec_method<75>(), rec_method<126>(), rec_method<131>(), rec_method<132>(), rec_method<142>(),
  };
  return m;
}

const Kind& list_kind() {
  static const Kind k("command list", kMaxSlots, list_methods());
  return k;
}

// ---- memory values: the GPU's writes at the bottom of the pipe, and its waits ----

struct ValueWrite {
  uint64_t address = 0, value = 0;
  uint32_t bytes = 4, flags = 0;  // flags 1: the GPU timestamp, 2: the system timestamp (64 bits)
};

// The last value our writes put at each address (to tell a wait whose writer never ran from one
// whose writer is behind).
std::mutex g_written_lock;
std::unordered_map<uint64_t, uint64_t> g_written;

void note_written(uint64_t address, uint64_t value) {
  std::lock_guard g(g_written_lock);
  g_written[address] = value;
}

void write_value(const ValueWrite& w) {
  uint64_t v = w.value;
  note_written(w.address, v);
  if (w.bytes == 8 && (w.flags & 3)) {  // both clocks tick at the queue's timestamp frequency, QPC's
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    v = static_cast<uint64_t>(t.QuadPart);
  }
  __try {
    if (w.bytes == 4) *reinterpret_cast<volatile uint32_t*>(w.address) = static_cast<uint32_t>(v);
    else *reinterpret_cast<volatile uint64_t*>(w.address) = v;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// The compute programs of each recent submission (by its host fence value), to name what the GPU
// is still running when it is slow.
std::mutex g_submitted_lock;
std::map<uint64_t, std::vector<std::string>> g_submitted;

void note_submitted(uint64_t fence, std::vector<std::string> programs) {
  std::lock_guard g(g_submitted_lock);
  g_submitted[fence] = std::move(programs);
  while (g_submitted.size() > 256) g_submitted.erase(g_submitted.begin());
}

// The programs of the submissions after `completed` up to `fence`.
std::string programs_between(uint64_t completed, uint64_t fence) {
  std::lock_guard g(g_submitted_lock);
  std::string text;
  size_t n = 0;
  for (auto it = g_submitted.upper_bound(completed); it != g_submitted.end() && it->first <= fence; ++it) {
    for (const std::string& m : it->second) {
      if (n++ < 16) text += " " + m.substr(0, 16);
    }
  }
  return text.empty() ? " (none)" : text;
}

// Writes wait for the host fence after the lists that hold them, then land in order, from a
// thread of ours: as on the console, a value is written once the work before it has run.
class ValueWriter {
 public:
  void add(uint64_t fence, std::vector<ValueWrite> writes) {
    std::lock_guard g(lock_);
    if (!started_) {
      started_ = true;
      std::thread([this] { run(); }).detach();
    }
    queue_.emplace_back(fence, std::move(writes));
    wake_.notify_one();
  }

 private:
  void run() {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (;;) {
      std::pair<uint64_t, std::vector<ValueWrite>> next;
      {
        std::unique_lock g(lock_);
        wake_.wait(g, [this] { return !queue_.empty(); });
        next = std::move(queue_.front());
        queue_.pop_front();
      }
      present::HostGpu* gpu = present::host_gpu();
      if (next.first && gpu && !gpu->done(next.first) && event) {
        gpu->fence->SetEventOnCompletion(next.first, event);
        if (WaitForSingleObject(event, 2000) == WAIT_TIMEOUT) {
          static std::atomic<int> logged{0};
          if (logged.fetch_add(1, std::memory_order_relaxed) < 4) {
            const uint64_t completed = gpu->fence->GetCompletedValue();
            logf("gpu: lists still running after 2 s (host fence at %llu of %llu); their memory value writes land anyway; "
                 "compute programs in them:%s",
                 static_cast<unsigned long long>(completed), static_cast<unsigned long long>(next.first),
                 programs_between(completed, next.first).c_str());
          }
        }
      }
      for (const ValueWrite& w : next.second) write_value(w);
    }
  }

  std::mutex lock_;
  std::condition_variable wake_;
  std::deque<std::pair<uint64_t, std::vector<ValueWrite>>> queue_;
  bool started_ = false;
};

ValueWriter& value_writer() {
  static ValueWriter* const w = new ValueWriter;
  return *w;
}

uint64_t read_value(uint64_t address, uint32_t bytes) {
  __try {
    return bytes == 4 ? *reinterpret_cast<volatile uint32_t*>(address) : *reinterpret_cast<volatile uint64_t*>(address);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// D3D12_COMPARISON_FUNC order: the wait ends when `value <op> reference`.
bool value_met(uint32_t comparison, uint64_t value, uint64_t reference) {
  switch (comparison) {
    case 1: return false;
    case 2: return value < reference;
    case 3: return value == reference;
    case 4: return value <= reference;
    case 5: return value > reference;
    case 6: return value != reference;
    case 7: return value >= reference;
    default: return true;
  }
}

// A list's waits hold its submission (the host queue cannot wait for memory): the values come
// from earlier lists' writes or from the CPU. A wait not met in 2 s is let go, and said.
void wait_for_values(const Recording& r) {
  for (const Command& c : r.commands) {
    if (c.op != kWaitValue32 && c.op != kWaitValue64) continue;
    const uint32_t bytes = c.op == kWaitValue32 ? 4 : 8;
    const uint64_t address = c.a[0], reference = bytes == 4 ? (c.a[2] & 0xFFFFFFFFu) : c.a[2];
    const uint32_t comparison = static_cast<uint32_t>(c.a[1]);
    if (address < 0x10000) continue;
    const ULONGLONG start = GetTickCount64();
    bool met = value_met(comparison, read_value(address, bytes), reference);
    while (!met && GetTickCount64() - start < 2000) {
      Sleep(0);
      met = value_met(comparison, read_value(address, bytes), reference);
    }
    if (!met) {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        std::string last = "never written by a list of ours";
        {
          std::lock_guard g(g_written_lock);
          if (auto it = g_written.find(address); it != g_written.end()) last = "last written " + std::to_string(it->second);
          size_t close_by = 0;
          for (const auto& [a, v] : g_written) close_by += a - (address & ~0xFFFull) < 0x2000;
          last += ", " + std::to_string(close_by) + " written addresses within its 8 KiB";
        }
        logf("gpu: a list waits for the value at 0x%s (comparison %u, reference %llu, now %llu; %s): not met in 2 s, let go",
             hex(address).c_str(), comparison, static_cast<unsigned long long>(reference),
             static_cast<unsigned long long>(read_value(address, bytes)), last.c_str());
      }
    }
  }
}

void collect_writes(const Recording& r, std::vector<ValueWrite>& out) {
  for (const Command& c : r.commands) {
    if ((c.op != kWriteValue32 && c.op != kWriteValue64) || c.a[0] < 0x10000) continue;
    ValueWrite w;
    w.address = c.a[0];
    w.bytes = c.op == kWriteValue32 ? 4 : 8;
    w.value = w.bytes == 4 ? (c.a[1] & 0xFFFFFFFFu) : c.a[1];
    w.flags = w.bytes == 8 ? static_cast<uint32_t>(c.a[3]) : 0;
    out.push_back(w);
  }
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
    case 26: return "ResourceBarrier";
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
    case kSetDepthBounds: return "OMSetDepthBounds";
    case kWriteValue32: return "WriteValue32";
    case kWriteValue64: return "WriteValue64";
    case kWaitValue32: return "WaitValue32";
    case kWaitValue64: return "WaitValue64";
    case kReset: return "Reset";
    case kBarrier: return "Barrier";
    default: return nullptr;
  }
}

const Recording* recording_of(const void* list) {
  Object* o = object_of_kind(list, list_kind());
  return o ? &static_cast<CommandList*>(o)->rec : nullptr;
}

void submit_lists(UINT count, void* const* lists) {
  if (!lists) return;
  present::HostGpu* gpu = present::host_gpu();
  std::vector<ID3D12CommandList*> host_lists;
  std::vector<HostList*> owners;
  std::vector<ValueWrite> writes;
  for (UINT i = 0; i < count; ++i) {
    Object* o = object_of_kind(lists[i], list_kind());
    if (!o) continue;
    const Recording& r = static_cast<CommandList*>(o)->rec;
    wait_for_values(r);
    collect_writes(r, writes);
    if (!gpu) continue;
    HostList& h = static_cast<CommandList*>(o)->host;
    if (h.open) host_close(h);  // the title submits only closed lists; be safe
    if (!h.ready) continue;
    host_lists.push_back(h.list.Get());
    owners.push_back(&h);
  }
  if (host_lists.empty()) {
    if (!writes.empty()) value_writer().add(0, std::move(writes));  // nothing runs on the host: in order, now
    return;
  }
  // Textures whose memory the title wrote before submitting are uploaded ahead of its lists.
  ID3D12CommandList* uploads = texture_uploads_for_submission();
  if (uploads) host_lists.insert(host_lists.begin(), uploads);
  const uint64_t done = gpu->submit(static_cast<UINT>(host_lists.size()), host_lists.data());
  {
    std::vector<std::string> programs;
    for (HostList* h : owners) {
      for (const std::string& m : h->dispatched) {
        if (std::find(programs.begin(), programs.end(), m) == programs.end()) programs.push_back(m);
      }
    }
    note_submitted(done, std::move(programs));
  }
  if (uploads) bindings_submitted(done, uploads);
  if (!writes.empty()) value_writer().add(done, std::move(writes));
  for (HostList* h : owners) {
    h->allocators[h->current].fence = done;
    h->ready = false;
    if (h->flushed) {  // this list holds the page table's and the bindings' upkeep
      page_table_submitted(done, h->list.Get());
      bindings_submitted(done, h->list.Get());
    }
  }
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
    logf("frame %llu: %u lists, %u commands: %s; draws so far on the GPU %llu, waiting for shaders %llu; dispatches %llu, "
         "waiting %llu",
         static_cast<unsigned long long>(f.frame), f.lists, f.commands, ops.c_str(),
         static_cast<unsigned long long>(g_draws_run.load()), static_cast<unsigned long long>(g_draws_skipped.load()),
         static_cast<unsigned long long>(g_dispatches_run.load()), static_cast<unsigned long long>(g_dispatches_skipped.load()));
    logf("frame %llu: compute pipelines %s", static_cast<unsigned long long>(f.frame), compute_pipeline_counts().c_str());
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
