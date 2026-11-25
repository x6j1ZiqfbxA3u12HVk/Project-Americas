#include "runtime/d3d12x/gpu_memory.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/text.h"
#include "runtime/log.h"
#include "runtime/memory/title_memory.h"
#include "runtime/present/host_gpu.h"

namespace am::rt::d3d12x {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kPage = 64 * 1024;
constexpr uint64_t kAddressBits = 40;
constexpr uint64_t kTableEntries = (uint64_t{1} << kAddressBits) / kPage;  // 16 Mi entries, 64 MiB
constexpr uint32_t kHeapSize = 65536;  // windows from index 1 up, texture views from the top down

struct Window {
  uint64_t base = 0, size = 0;
  ComPtr<ID3D12Heap> heap;
  ComPtr<ID3D12Resource> buffer;
};

struct GpuMemory {
  std::mutex lock;
  bool started = false, usable = false;
  ComPtr<ID3D12Device3> device;
  ComPtr<ID3D12DescriptorHeap> heap;
  ComPtr<ID3D12RootSignature> root, graphics_root;
  ComPtr<ID3D12Resource> table;   // default heap, R32 entries
  // Uploads of entries the GPU may still copy from, each with the fence value after the lists
  // that copy them (kPending until those are submitted).
  static constexpr uint64_t kPending = UINT64_MAX;
  struct Upload {
    uint64_t fence;
    const void* list;  // the list that copies from it
    ComPtr<ID3D12Resource> resource;
  };
  std::vector<Upload> uploads;
  std::vector<Window> windows;
  // Ranges of entries changed since the last flush, and their values. Its own lock: the title
  // memory's view listener adds to it while holding the title memory's lock.
  std::mutex pending_lock;
  std::vector<std::pair<uint64_t, std::vector<uint32_t>>> pending;
  // The pool's views on the CPU side (first page -> pages, first pool page), for host addresses.
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> pool_views;
  UINT step = 0;
  uint32_t next_view = kHeapSize;  // the lowest index handed out to a texture view so far
  uint32_t pool_window = 0;  // the committed physical page pool's window, once opened

  bool start() {
    started = true;
    present::HostGpu* gpu = present::host_gpu();
    if (!gpu || FAILED(gpu->device.As(&device))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return fail("no shader-visible heap");
    step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, kTableEntries * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&table)))) {
      return fail("no page table");
    }
    table->SetName(L"page table");
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_TYPELESS;
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Buffer.NumElements = static_cast<UINT>(kTableEntries);
    sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(table.Get(), &sv, heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER1 params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, kUserDataConstants};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;  // the binding table, u0
    params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{};
    rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rd.Desc_1_1 = {2, params, 0, nullptr,
                   D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED |
                       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rd, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
      return fail("no root signature for translated programs");
    }
    D3D12_ROOT_PARAMETER1 stages[3]{};
    stages[0].ParameterType = stages[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    stages[0].Constants = stages[1].Constants = {0, 0, kUserDataConstants};
    stages[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    stages[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    stages[2] = params[1];  // the binding table, u0, every stage
    rd.Desc_1_1 = {3, stages, 0, nullptr, rd.Desc_1_1.Flags};
    blob.Reset();
    if (FAILED(D3D12SerializeVersionedRootSignature(&rd, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&graphics_root)))) {
      return fail("no graphics root signature for translated programs");
    }
    windows.emplace_back();  // window numbers start at 1: entry 0 means none
    usable = true;
    return true;
  }

  bool fail(const char* why) {
    logf("gpu memory: %s", why);
    return false;
  }

  // The title allocation around `address`: its base and size (one host allocation).
  static bool allocation(uint64_t address, uint64_t& base, uint64_t& size) {
    MEMORY_BASIC_INFORMATION m;
    if (VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof m) != sizeof m || m.State != MEM_COMMIT ||
        m.Type != MEM_PRIVATE) {
      return false;
    }
    base = reinterpret_cast<uint64_t>(m.AllocationBase);
    uint64_t p = base;
    while (VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof m) == sizeof m && reinterpret_cast<uint64_t>(m.AllocationBase) == base &&
           m.State != MEM_FREE) {
      p = reinterpret_cast<uint64_t>(m.BaseAddress) + m.RegionSize;
    }
    size = p - base;
    return size != 0;
  }

  bool open(uint64_t address) {
    for (size_t n = 1; n < windows.size(); ++n) {
      if (n != pool_window && address - windows[n].base < windows[n].size) return true;
    }
    uint64_t base = 0, size = 0;
    if ((address >> kAddressBits) || !allocation(address, base, size)) return false;
    if (base % kPage || (base + size) >> kAddressBits || size > (uint64_t{0xFFFF} + 1) * kPage) return false;
    if (windows.size() + 1 >= next_view) return fail("no room for more windows");
    Window w;
    w.base = base;
    w.size = size;
    if (FAILED(device->OpenExistingHeapFromAddress(reinterpret_cast<void*>(base), IID_PPV_ARGS(&w.heap)))) {
      logf("gpu memory: cannot open the allocation at 0x%s (%s) for the GPU", hex(base).c_str(), format_bytes(size).c_str());
      return false;
    }
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                           D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(device->CreatePlacedResource(w.heap.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&w.buffer)))) {
      return fail("no buffer over an opened allocation");
    }
    w.buffer->SetName(widen("window over 0x" + hex(base) + " (" + format_bytes(size) + ")").c_str());
    const uint32_t n = add_window(std::move(w));
    static std::atomic<int> told{0};
    if (told.fetch_add(1, std::memory_order_relaxed) < 64) {
      logf("gpu memory: window %u over the allocation at 0x%s (%s)", n, hex(base).c_str(), format_bytes(size).c_str());
    }
    std::vector<uint32_t> entries(size / kPage);
    for (uint32_t k = 0; k < entries.size(); ++k) entries[k] = (n << 16) | k;
    std::lock_guard p(pending_lock);
    pending.emplace_back(base / kPage, std::move(entries));
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 32) logf("gpu memory: window %u over 0x%s, %s", n, hex(base).c_str(), format_bytes(size).c_str());
    return true;
  }

  // A window's raw view at its index; returns the index.
  uint32_t add_window(Window w) {
    const uint32_t n = static_cast<uint32_t>(windows.size());
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = static_cast<UINT>(w.size / 4);
    uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T{n} * step;
    device->CreateUnorderedAccessView(w.buffer.Get(), nullptr, &uv, h);
    windows.push_back(std::move(w));
    return n;
  }

  // The committed physical page pool as one window; console views of its pages reach the page
  // table through the title memory's listener.
  void open_pool(HANDLE section, uint64_t pages) {
    if (pool_window || !section || !pages || pages > 0x10000) return;
    Window w;
    w.size = pages * kPage;
    // A mirror view of the whole section, opened by address: it sees what every view of the
    // pool's pages holds. (Opening the section itself would lose its commit charge for good on
    // some drivers; see pool_window_wanted.)
    void* mirror = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, w.size);
    if (!mirror || FAILED(device->OpenExistingHeapFromAddress(mirror, IID_PPV_ARGS(&w.heap)))) {
      if (mirror) UnmapViewOfFile(mirror);
      fail("cannot open the physical page pool for the GPU");
      pool_window = UINT32_MAX;
      return;
    }
    w.base = reinterpret_cast<uint64_t>(mirror);  // not a title address: kept only for the record
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, w.size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                           D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    if (FAILED(device->CreatePlacedResource(w.heap.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&w.buffer)))) {
      fail("no buffer over the physical page pool");
      pool_window = UINT32_MAX;
      return;
    }
    w.buffer->SetName(L"window over the physical page pool");
    pool_window = add_window(std::move(w));
    logf("gpu memory: window %u over the physical page pool, %s", pool_window, format_bytes(pages * kPage).c_str());
  }

  // A heap index for a texture view, from the top down; 0 when the heap is full.
  uint32_t allocate_view(D3D12_CPU_DESCRIPTOR_HANDLE* handle) {
    if (next_view <= windows.size() + 1) return 0;
    const uint32_t n = --next_view;
    if (handle) {
      *handle = heap->GetCPUDescriptorHandleForHeapStart();
      handle->ptr += SIZE_T{n} * step;
    }
    return n;
  }

  void flush(ID3D12GraphicsCommandList* list) {
    std::lock_guard drain(pending_lock);
    if (present::HostGpu* gpu = present::host_gpu()) {
      std::erase_if(uploads, [gpu](const Upload& u) { return u.fence != kPending && gpu->done(u.fence); });
    }
    if (pending.empty()) return;
    size_t bytes = 0;
    for (const auto& [first, entries] : pending) bytes += entries.size() * 4;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                           D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&upload)))) {
      return;
    }
    upload->SetName(L"page table entries");
    uint8_t* p = nullptr;
    upload->Map(0, nullptr, reinterpret_cast<void**>(&p));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {table.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, &b);
    size_t at = 0;
    for (const auto& [first, entries] : pending) {
      std::memcpy(p + at, entries.data(), entries.size() * 4);
      list->CopyBufferRegion(table.Get(), first * 4, upload.Get(), at, entries.size() * 4);
      at += entries.size() * 4;
    }
    upload->Unmap(0, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    pending.clear();
    uploads.push_back({kPending, list, std::move(upload)});
  }
};

GpuMemory& memory() {
  static GpuMemory* const m = new GpuMemory;
  return *m;
}

GpuMemory* ready() {
  GpuMemory& m = memory();
  if (!m.started) m.start();
  return m.usable ? &m : nullptr;
}

// The title memory's listener: views of physical pages mapped or unmapped.
void on_view(uint64_t address, uint64_t pages, int64_t pool_page) {
  GpuMemory& m = memory();
  if (!m.pool_window || m.pool_window == UINT32_MAX || (address >> kAddressBits)) return;
  std::vector<uint32_t> entries(pages, 0u);
  if (pool_page >= 0) {
    for (uint64_t k = 0; k < pages; ++k) entries[k] = (m.pool_window << 16) | static_cast<uint32_t>(pool_page + k);
  }
  std::lock_guard p(m.pending_lock);
  const uint64_t first = address / kPage;
  m.pool_views.erase(m.pool_views.lower_bound(first), m.pool_views.lower_bound(first + pages));
  if (pool_page >= 0) m.pool_views[first] = {pages, static_cast<uint64_t>(pool_page)};
  m.pending.emplace_back(first, std::move(entries));
}

// Opens the pool once it exists (the title makes it when it first asks for pages). The section
// is asked for before the GPU memory's lock is taken: the title memory's lock comes first.
// The pool window. OpenExistingHeapFromFileMapping on the pool's section lost the section's
// whole commit charge for good on the development PC's AMD driver (a gigabyte a run, even with
// everything released before exit, until the system ran out of commit and the device was
// lost), so open_pool opens a mirror view by address instead, which loses nothing.
// AMERICAS_NO_GPU_POOL_WINDOW=1 leaves the pool without a window (shaders read zeros there).
bool pool_window_wanted() {
  static const bool wanted = GetEnvironmentVariableA("AMERICAS_NO_GPU_POOL_WINDOW", nullptr, 0) == 0;
  return wanted;
}

void open_pool_if_ready() {
  GpuMemory& m = memory();
  if (m.pool_window) return;
  if (!pool_window_wanted()) {
    std::lock_guard g(m.lock);
    if (!m.pool_window) {
      m.pool_window = UINT32_MAX;
      logf("gpu memory: no window over the physical page pool (AMERICAS_NO_GPU_POOL_WINDOW)");
    }
    return;
  }
  mem::TitleMemory& tm = mem::title_memory();
  HANDLE section = tm.gpu_pool_section();
  const uint64_t pages = tm.gpu_pool_pages();
  if (!section) return;
  {
    std::lock_guard g(m.lock);
    GpuMemory* r = ready();
    if (!r || r->pool_window) return;
    r->open_pool(section, pages);
    if (!r->pool_window || r->pool_window == UINT32_MAX) return;
  }
  tm.set_view_listener(&on_view);  // tells the views there are now, then every change
}

}  // namespace

bool open_gpu_window(uint64_t address) {
  std::lock_guard g(memory().lock);
  GpuMemory* m = ready();
  return m && m->open(address);
}

ID3D12DescriptorHeap* translated_shader_heap() {
  std::lock_guard g(memory().lock);
  GpuMemory* m = ready();
  return m ? m->heap.Get() : nullptr;
}

ID3D12RootSignature* translated_root_signature() {
  std::lock_guard g(memory().lock);
  GpuMemory* m = ready();
  return m ? m->root.Get() : nullptr;
}

ID3D12RootSignature* translated_graphics_root_signature() {
  std::lock_guard g(memory().lock);
  GpuMemory* m = ready();
  return m ? m->graphics_root.Get() : nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS host_gpu_address(uint64_t address, uint64_t size) {
  if (!size || (address >> kAddressBits)) return 0;
  open_pool_if_ready();
  std::lock_guard g(memory().lock);
  GpuMemory* m = ready();
  if (!m) return 0;
  if (m->open(address)) {
    for (size_t n = 1; n < m->windows.size(); ++n) {
      const Window& w = m->windows[n];
      if (n != m->pool_window && address - w.base < w.size && address + size - w.base <= w.size) {
        return w.buffer->GetGPUVirtualAddress() + (address - w.base);
      }
    }
  }
  if (!m->pool_window || m->pool_window == UINT32_MAX) return 0;
  std::lock_guard p(m->pending_lock);
  const uint64_t page = address / kPage, last = (address + size - 1) / kPage;
  auto it = m->pool_views.upper_bound(page);
  if (it == m->pool_views.begin()) return 0;
  --it;
  const auto& [first, view] = *it;
  if (page - first >= view.first || last - first >= view.first) return 0;  // not all in one view
  return m->windows[m->pool_window].buffer->GetGPUVirtualAddress() + (view.second + (page - first)) * kPage + address % kPage;
}

uint32_t allocate_view_slot(D3D12_CPU_DESCRIPTOR_HANDLE* handle) {
  std::lock_guard g(memory().lock);
  GpuMemory* m = ready();
  return m ? m->allocate_view(handle) : 0;
}

void map_gpu_pages(uint64_t address, const int64_t* pages, uint64_t count) {
  open_pool_if_ready();
  GpuMemory& m = memory();
  if (!m.pool_window || m.pool_window == UINT32_MAX || (address >> kAddressBits) || address % kPage) return;
  std::vector<uint32_t> entries(count, 0u);
  for (uint64_t k = 0; k < count; ++k) {
    if (pages[k] >= 0) entries[k] = (m.pool_window << 16) | static_cast<uint32_t>(pages[k]);
  }
  std::lock_guard p(m.pending_lock);
  m.pending.emplace_back(address / kPage, std::move(entries));
}

void page_table_submitted(uint64_t fence_value, const void* list) {
  std::lock_guard g(memory().lock);
  for (auto& u : memory().uploads) {
    if (u.fence == GpuMemory::kPending && u.list == list) u.fence = fence_value;
  }
}

void flush_page_table(ID3D12GraphicsCommandList* list) {
  open_pool_if_ready();
  std::lock_guard g(memory().lock);
  if (GpuMemory* m = ready()) m->flush(list);
}

}  // namespace am::rt::d3d12x
