// The console graphics objects, called through their vtables the way title code calls them.

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "runtime/d3d12x/commands.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/bindings.h"
#include "runtime/d3d12x/gpu_memory.h"
#include "runtime/memory/title_memory.h"
#include "runtime/d3d12x/host.h"
#include "host/dxc_compile.h"
#include "shader/translate.h"
#include <wrl/client.h>
#include <cstdio>
#include "runtime/present/host_gpu.h"
#include "runtime/present/presenter.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/d3d12x/object.h"
#include "runtime/d3d12x/root_signature.h"
#include "test.h"

using namespace am::rt::d3d12x;

namespace {

template <class Fn>
Fn slot(void* object, int index) {
  return reinterpret_cast<Fn>((*static_cast<void* const* const*>(object))[index]);
}

void* make_device() {
  uint32_t params[20] = {0x20004};
  void* device = nullptr;
  create_device(nullptr, params, nullptr, &device);
  return device;
}

const GUID kIDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};

}  // namespace

TEST_CASE(d3d12x_device_and_its_dxgi_view) {
  void* device = make_device();
  CHECK(device != nullptr);
  void* dxgi = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const GUID*, void**)>(device, 0)(device, &kIDXGIDevice, &dxgi) == S_OK);
  CHECK(dxgi != nullptr && dxgi != device);
  void* adapter = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, void**)>(dxgi, 7)(dxgi, &adapter) == S_OK && adapter);
  void* output = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT, void**)>(adapter, 7)(adapter, 0, &output) == S_OK && output);
  void* none = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT, void**)>(adapter, 7)(adapter, 1, &none) == DXGI_ERROR_NOT_FOUND);
  UINT count = 0;
  CHECK(slot<HRESULT(WINAPI*)(void*, DXGI_FORMAT, UINT, UINT*, void*)>(output, 8)(output, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &count,
                                                                                 nullptr) == S_OK);
  CHECK(count == 1);
}

TEST_CASE(d3d12x_descriptor_sizes_and_heap_over_title_memory) {
  void* device = make_device();
  using IncrementFn = UINT(WINAPI*)(void*, UINT);
  CHECK(slot<IncrementFn>(device, 15)(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) == 32);
  CHECK(slot<IncrementFn>(device, 15)(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) == 16);

  std::vector<uint8_t> memory(64 * 32);
  struct {
    uint32_t type, count, flags, node_mask;
    uint64_t address, size;
  } desc{0, 64, 1, 1, reinterpret_cast<uint64_t>(memory.data()), memory.size()};
  void* heap = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 14)(device, &desc, nullptr, &heap) == S_OK);
  CHECK(heap != nullptr);
  const uint64_t start = slot<uint64_t(WINAPI*)(void*)>(heap, 9)(heap);
  CHECK(start == reinterpret_cast<uint64_t>(memory.data()));
  CHECK(slot<uint64_t(WINAPI*)(void*)>(heap, 10)(heap) == start);

  // D3D12's 16-byte description followed by junk: the heap's memory is ours.
  struct {
    uint32_t type, count, flags, node_mask;
    uint64_t address, size;
  } junk{0, 64, 0, 1, reinterpret_cast<uint64_t>(memory.data()) + 8, uint64_t{1} << 33};
  void* own = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 14)(device, &junk, nullptr, &own) == S_OK);
  const uint64_t own_start = slot<uint64_t(WINAPI*)(void*)>(own, 9)(own);
  CHECK(own_start != 0 && own_start != junk.address);
  junk.address = 0xf00;
  junk.size = 0;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 14)(device, &junk, nullptr, &own) == S_OK);
  CHECK(slot<uint64_t(WINAPI*)(void*)>(own, 9)(own) != 0xf00);

  // A view leaves a record in its slot; copying descriptors copies the record.
  slot<void(WINAPI*)(void*, void*, const void*, uint64_t)>(device, 18)(device, nullptr, nullptr, start + 32);
  uint32_t magic = 0;
  std::memcpy(&magic, memory.data() + 32, 4);
  CHECK(magic == 0x56444D41);
  slot<void(WINAPI*)(void*, UINT, uint64_t, uint64_t, UINT)>(device, 24)(device, 1, start + 5 * 32, start + 32, 0);
  CHECK(std::memcmp(memory.data() + 5 * 32, memory.data() + 32, 32) == 0);
}

TEST_CASE(d3d12x_resource_allocation_info) {
  void* device = make_device();
  using InfoFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                          const D3D12_RESOURCE_DESC*);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 100;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  D3D12_RESOURCE_ALLOCATION_INFO info{};
  slot<InfoFn>(device, 25)(&info, device, 1, 1, &buffer);  // the console order: return buffer first
  CHECK(info.SizeInBytes == 65536 && info.Alignment == 65536);

  D3D12_RESOURCE_DESC tiny{};
  tiny.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tiny.Alignment = 4096;
  tiny.Width = 64;
  tiny.Height = 64;
  tiny.DepthOrArraySize = 1;
  tiny.MipLevels = 1;
  tiny.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tiny.SampleDesc.Count = 1;
  slot<InfoFn>(device, 25)(&info, device, 1, 1, &tiny);
  CHECK(info.Alignment == 4096 && info.SizeInBytes == 16384);
  tiny.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;  // render targets never get tiny alignment
  slot<InfoFn>(device, 25)(&info, device, 1, 1, &tiny);
  CHECK(info.Alignment == 65536);

  D3D12_RESOURCE_DESC bc{};
  bc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  bc.Width = 256;
  bc.Height = 256;
  bc.DepthOrArraySize = 1;
  bc.MipLevels = 1;
  bc.Format = DXGI_FORMAT_BC1_UNORM;  // 64 x 64 elements of 8 bytes: 4 KiB blocks of 32 x 16
  bc.SampleDesc.Count = 1;
  slot<InfoFn>(device, 25)(&info, device, 1, 1, &bc);
  CHECK(info.SizeInBytes == 32768 && info.Alignment == 4096);
}

TEST_CASE(d3d12x_texture_layouts) {
  // The game's back buffer: 64 KiB blocks of 128 x 128 texels, 30 x 17 of them.
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = 3840;
  d.Height = 2160;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  TextureLayout t;
  CHECK(texture_layout(d, t));
  CHECK(t.swizzle_mode == 27 && t.block_width == 128 && t.block_height == 128);
  CHECK(t.size == 30ull * 17 * 65536 && t.alignment == 65536);

  // A named mode wins: 0x100 + 0 is linear, pitch in 256-byte units.
  d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x100);
  CHECK(texture_layout(d, t) && t.swizzle_mode == 0 && t.subresources[0].row_pitch == 15360);

  // Mips and layers: a 256 x 256 BC3 array of 2 with a full chain, 64 KiB blocks of 64 x 64
  // elements (16 bytes each). Mip 0 fills a block; mips 1-8 (32 elements and less) share the
  // mip tail, which comes first: each layer is the tail block, then mip 0.
  D3D12_RESOURCE_DESC a{};
  a.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  a.Width = 256;
  a.Height = 256;
  a.DepthOrArraySize = 2;
  a.MipLevels = 0;
  a.Format = DXGI_FORMAT_BC3_UNORM;
  a.SampleDesc.Count = 1;
  a.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x100 | 25);
  CHECK(texture_layout(a, t));
  CHECK(t.mips == 9 && t.layers == 2 && t.subresources.size() == 18);
  CHECK(t.first_tail_mip == 1 && t.subresources[0].offset == 65536 && t.subresources[9].offset == 3 * 65536);
  CHECK(t.subresources[1].in_tail && t.subresources[1].offset == 0 && t.subresources[10].offset == 2 * 65536);
  CHECK(t.subresources[0].width_elements == 64 && t.subresources[8].width_elements == 1);
  CHECK(t.size == 4 * 65536);
  // Inside the tail, the largest tail mip (slot 11) starts at byte 16 << 11 = 0x8000: address
  // bit 15, an x bit (odd bits from 9 are x, even ones from 8 are y, in 256-byte tiles of 4 x 4
  // elements), so element (32, 0); the smallest (mip 8, slot 4) at byte 0x400, element (0, 8).
  CHECK(t.subresources[1].tail_x == 32 && t.subresources[1].tail_y == 0);
  CHECK(t.subresources[8].tail_x == 0 && t.subresources[8].tail_y == 8);

  // The UI streamer's textures: 1024 x 1024 BC7, 9 mips, 64 KiB blocks (mode 10). Mips 3-8 are
  // in the tail: 64 + 64 + 256 + 1024 KiB, mip 0 last.
  D3D12_RESOURCE_DESC ui{};
  ui.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  ui.Width = 1024;
  ui.Height = 1024;
  ui.DepthOrArraySize = 1;
  ui.MipLevels = 9;
  ui.Format = DXGI_FORMAT_BC7_UNORM;
  ui.SampleDesc.Count = 1;
  ui.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x100 | 10);
  CHECK(texture_layout(ui, t));
  CHECK(t.first_tail_mip == 3 && t.size == 1408 * 1024);
  CHECK(t.subresources[2].offset == 65536 && t.subresources[1].offset == 2 * 65536 && t.subresources[0].offset == 6 * 65536);

  // XG: a suggestion fills the Layout field; the computer copies linear rows into the layout.
  D3D12_RESOURCE_DESC small_tex{};
  small_tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  small_tex.Width = 40;
  small_tex.Height = 3;
  small_tex.DepthOrArraySize = 1;
  small_tex.MipLevels = 1;
  small_tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  small_tex.SampleDesc.Count = 1;
  UINT layout = 0;
  CHECK(xg_suggest_swizzle_mode(&small_tex, 0, &layout) == S_OK && layout == (0x100 | 21));
  small_tex.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
  void* computer = nullptr;
  CHECK(xg_create_texture_computer(&small_tex, &computer) == S_OK && computer);
  std::vector<uint8_t> linear(3 * 160), memory(4096, 0xEE);
  for (size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<uint8_t>(i);
  using CopyFn = HRESULT(WINAPI*)(void*, void*, UINT, UINT, const void*, UINT, UINT);
  CHECK(slot<CopyFn>(computer, 8)(computer, memory.data(), 0, 0, linear.data(), 160, 480) == S_OK);
  // 4 KiB blocks of 32 x 32 texels: the pitch is 64 texels (256 bytes).
  CHECK(memory[0] == 0 && memory[159] == 159 && memory[160] == 0xEE && memory[256] == 160 && memory[512 + 159] == 0xDF);
  CHECK(slot<ULONG(WINAPI*)(void*)>(computer, 1)(computer) == 0);
}

TEST_CASE(d3d12x_queue_signals_fence_at_once) {
  void* device = make_device();
  D3D12_COMMAND_QUEUE_DESC qd{};
  void* queue = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 8)(device, &qd, nullptr, &queue) == S_OK);
  void* fence = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64, UINT, const GUID*, void**)>(device, 36)(device, 0, 0, nullptr, &fence) == S_OK);
  CHECK(slot<HRESULT(WINAPI*)(void*, void*, UINT64)>(queue, 16)(queue, fence, 5) == S_OK);  // the console's Signal
  CHECK(slot<UINT64(WINAPI*)(void*)>(fence, 8)(fence) == 5);
  const volatile uint64_t* word = slot<const volatile uint64_t*(WINAPI*)(void*)>(fence, 12)(fence);
  CHECK(word && *word == 5);  // the value in memory, as the game's wait loop reads it
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64, HANDLE)>(fence, 9)(fence, 7, event) == S_OK);
  CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);  // not reached yet
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64)>(fence, 10)(fence, 7) == S_OK);
  CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
  CloseHandle(event);
  UINT64 frequency = 0;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64*)>(queue, 18)(queue, &frequency) == S_OK && frequency > 0);
  CHECK(slot<ULONG(WINAPI*)(void*)>(fence, 2)(fence) == 0);  // released and freed
}

// The lists' copies, done in title memory at submission: a buffer region, a whole buffer, and
// rows of a buffer footprint into a texture subresource (texels linear in it, layout.h).
TEST_CASE(d3d12x_copies_in_title_memory) {
  void* device = make_device();
  using CreateFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*, UINT,
                                    const D3D12_CLEAR_VALUE*, void*, UINT, const DXGI_FORMAT*, const GUID*, void**);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 65536;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = 64;
  td.Height = 64;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  void *a = nullptr, *b = nullptr, *t = nullptr;
  CHECK(slot<CreateFn>(device, 127)(device, &heap, 0, &bd, 0, nullptr, nullptr, 0, nullptr, nullptr, &a) == S_OK);
  CHECK(slot<CreateFn>(device, 127)(device, &heap, 0, &bd, 0, nullptr, nullptr, 0, nullptr, nullptr, &b) == S_OK);
  CHECK(slot<CreateFn>(device, 127)(device, &heap, 0, &td, 0, nullptr, nullptr, 0, nullptr, nullptr, &t) == S_OK);
  if (!a || !b || !t) return;
  D3D12_RESOURCE_DESC desc{};
  uint64_t pa = 0, pb = 0, pt = 0;
  CHECK(resource_memory(a, &desc, &pa) && resource_memory(b, &desc, &pb) && resource_memory(t, &desc, &pt));
  auto* bytes_a = reinterpret_cast<uint8_t*>(pa);
  for (uint32_t k = 0; k < 65536; ++k) bytes_a[k] = static_cast<uint8_t>(k * 7 + 1);

  Recording r;
  Command region;
  region.op = kCopyBufferRegion;
  region.a[0] = reinterpret_cast<uint64_t>(b);
  region.a[1] = 100;
  region.a[2] = reinterpret_cast<uint64_t>(a);
  region.a[3] = 1000;
  region.a[4] = 50;
  r.commands.push_back(region);
  // 16 x 8 texels of a footprint (row pitch 256 bytes) to texel (4, 2) of the texture.
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = static_cast<ID3D12Resource*>(t);
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  src.pResource = static_cast<ID3D12Resource*>(a);
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Offset = 512;
  src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 16, 8, 1, 256};
  Command tex;
  tex.op = kCopyTextureRegion;
  tex.a[0] = reinterpret_cast<uint64_t>(&dst);
  tex.a[1] = 4;
  tex.a[2] = 2;
  tex.a[4] = reinterpret_cast<uint64_t>(&src);
  tex.data_at = static_cast<uint32_t>(r.data.size());
  tex.data_size = 2 * sizeof(D3D12_TEXTURE_COPY_LOCATION);
  r.data.resize(r.data.size() + tex.data_size);
  std::memcpy(r.data.data() + tex.data_at, &dst, sizeof dst);
  std::memcpy(r.data.data() + tex.data_at + sizeof dst, &src, sizeof src);
  r.commands.push_back(tex);
  run_copies(r);

  const auto* bytes_b = reinterpret_cast<const uint8_t*>(pb);
  bool ok = true;
  for (uint32_t k = 0; k < 50; ++k) ok = ok && bytes_b[100 + k] == bytes_a[1000 + k];
  CHECK(ok && bytes_b[99] == 0 && bytes_b[150] == 0);
  TextureLayout l;
  CHECK(texture_layout(td, l));
  const SubresourceLayout& s0 = l.subresources[0];
  const auto* texels = reinterpret_cast<const uint8_t*>(pt) + s0.offset;
  ok = true;
  for (uint32_t y = 0; y < 8; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      ok = ok && texels[(2 + y) * s0.row_pitch + (4 * 4) + x] == bytes_a[512 + y * 256 + x];
    }
  }
  CHECK(ok && texels[(2 + 8) * s0.row_pitch + 16] == 0 && texels[2 * s0.row_pitch + 15] == 0);

  // A whole buffer.
  Recording whole;
  Command all;
  all.op = kCopyResource;
  all.a[0] = reinterpret_cast<uint64_t>(b);
  all.a[1] = reinterpret_cast<uint64_t>(a);
  whole.commands.push_back(all);
  run_copies(whole);
  CHECK(std::memcmp(bytes_b, bytes_a, 65536) == 0);
  for (void* x : {a, b, t}) slot<ULONG(WINAPI*)(void*)>(x, 2)(x);
}

TEST_CASE(d3d12x_committed_resource_memory) {
  void* device = make_device();
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 3840;
  desc.Height = 2160;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  using CreateFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*, UINT,
                                    const D3D12_CLEAR_VALUE*, void*, UINT, const DXGI_FORMAT*, const GUID*, void**);
  void* resource = nullptr;
  CHECK(slot<CreateFn>(device, 127)(device, &heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &desc, 3, nullptr, nullptr, 0, nullptr,
                                     nullptr, &resource) == S_OK);
  CHECK(resource != nullptr);

  // Unified memory: the GPU address is where Map puts the CPU.
  const uint64_t gpu = slot<uint64_t(WINAPI*)(void*)>(resource, 11)(resource);
  void* cpu = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT, const D3D12_RANGE*, void**)>(resource, 8)(resource, 0, nullptr, &cpu) == S_OK);
  CHECK(gpu != 0 && reinterpret_cast<uint64_t>(cpu) == gpu && gpu % 65536 == 0);
  CHECK(static_cast<const uint8_t*>(cpu)[33177600 - 1] == 0);  // zeroed, and as large as it says

  D3D12_RESOURCE_DESC back{};
  slot<D3D12_RESOURCE_DESC*(WINAPI*)(D3D12_RESOURCE_DESC*, void*)>(resource, 10)(&back, resource);
  CHECK(back.Width == 3840 && back.Height == 2160 && back.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(slot<ULONG(WINAPI*)(void*)>(resource, 2)(resource) == 0);

  // The attributes the title's hooks dispatch on: the graphics runtime's allocator, large pages
  // for big resources, the alignment as a power of two.
  const uint64_t big = resource_attributes(33226752, 65536);
  CHECK(((big >> 16) & 0xFF) == 128 && ((big >> 14) & 3) == 2 && ((big >> 24) & 0x1F) == 16);
  const uint64_t medium = resource_attributes(65536, 65536);
  CHECK(((medium >> 14) & 3) == 1);
  CHECK(((resource_attributes(4096, 4096) >> 14) & 3) == 0);
}

TEST_CASE(d3d12x_root_signature_round_trip) {
  // A 1.1 root signature: a table of two ranges, root constants, a root UAV, a static sampler,
  // and a console flag bit, which must pass through.
  D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 3, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
  };
  D3D12_ROOT_PARAMETER1 params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {5, 0, 12};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].Descriptor = {7, 2, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.MaxLOD = 1000.0f;
  sampler.ShaderRegister = 9;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1 = {3, params, 1, &sampler, static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x3000000)};

  void* blob = nullptr;
  void* error = reinterpret_cast<void*>(1);
  CHECK(serialize_versioned_root_signature(&desc, &blob, &error) == S_OK);
  CHECK(blob != nullptr && error == nullptr);
  const void* data = slot<void*(WINAPI*)(void*)>(blob, 3)(blob);
  const SIZE_T size = slot<SIZE_T(WINAPI*)(void*)>(blob, 4)(blob);
  // 24 header + 3 x 12 parameters + (8 + 2 x 24) table + 12 constants + 12 descriptor + 52 sampler
  CHECK(size == 24 + 36 + 56 + 12 + 12 + 52);

  RootSignatureDesc back;
  CHECK(parse_root_signature(data, size, back));
  CHECK(back.version == 2 && back.flags == 0x3000000);
  CHECK(back.parameters.size() == 3 && back.samplers.size() == 1);
  CHECK(back.parameters[0].ranges.size() == 2 && back.parameters[0].ranges[1].base_register == 3);
  CHECK(back.parameters[0].ranges[1].table_offset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND);
  CHECK(back.parameters[0].ranges[0].flags == D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
  CHECK(back.parameters[1].constants == 12 && back.parameters[1].visibility == D3D12_SHADER_VISIBILITY_PIXEL);
  CHECK(back.parameters[2].shader_register == 7 && back.parameters[2].space == 2);
  CHECK(back.samplers[0].ShaderRegister == 9 && back.samplers[0].MaxLOD == 1000.0f);

  // The device takes the blob back.
  void* device = make_device();
  void* rs = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT, const void*, SIZE_T, const GUID*, void**)>(device, 16)(device, 1, data, size,
                                                                                               nullptr, &rs) == S_OK);
  CHECK(rs != nullptr);
  CHECK(slot<ULONG(WINAPI*)(void*)>(blob, 2)(blob) == 0);

  // A bad description fails with an error blob.
  desc.Version = static_cast<D3D_ROOT_SIGNATURE_VERSION>(9);
  CHECK(serialize_versioned_root_signature(&desc, &blob, &error) == E_INVALIDARG);
  CHECK(blob == nullptr && error != nullptr);
}

TEST_CASE(d3d12x_frame_events_pace_frames) {
  void* device = make_device();
  using IntervalFn = HRESULT(WINAPI*)(void*, void*, UINT, UINT, UINT);
  using WaitFn = HRESULT(WINAPI*)(void*, UINT, UINT, uint64_t, UINT, uint64_t*);
  CHECK(slot<IntervalFn>(device, 78)(device, nullptr, 10000, 1, 0) == S_OK);  // 10 ms frames
  uint64_t first = 0, second = 0, third = 0;
  CHECK(slot<WaitFn>(device, 80)(device, 0, 0, 0, 2, &first) == S_OK);
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&a);
  CHECK(slot<WaitFn>(device, 80)(device, 0, 0, 0, 2, &second) == S_OK);
  // Another event type sees the frame that has begun, without waiting for the next.
  uint64_t other = 0;
  CHECK(slot<WaitFn>(device, 80)(device, 2, INFINITE, 0, 0, &other) == S_OK);
  CHECK(other == second);
  CHECK(slot<WaitFn>(device, 80)(device, 0, 0, 0, 2, &third) == S_OK);
  QueryPerformanceCounter(&b);
  const double ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
  CHECK(second == first + 1 && third == second + 1);  // tokens count frames
  CHECK(ms > 15.0 && ms < 40.0);                       // two frame starts, 10 ms apart
}

TEST_CASE(d3d12x_command_lists_record_commands) {
  void* device = make_device();
  const uint32_t desc[4] = {0, 2, 1, 0};
  void* list = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const uint32_t*, void*, void*, const GUID*, void**)>(device, 73)(
            device, desc, nullptr, nullptr, nullptr, &list) == S_OK);
  CHECK(list != nullptr);
  // The console's slots: DrawInstanced 12, OMSetRenderTargets 50, ClearRenderTargetView 52,
  // Close 9, Reset 139.
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  slot<void(WINAPI*)(void*, uint64_t, const float*, UINT, const D3D12_RECT*)>(list, 52)(list, 0x1234, color, 0, nullptr);
  const uint64_t targets[2] = {0x100, 0x140};
  const uint64_t depth = 0x200;
  slot<void(WINAPI*)(void*, UINT, const uint64_t*, BOOL, const uint64_t*)>(list, 50)(list, 2, targets, FALSE, &depth);
  slot<void(WINAPI*)(void*, UINT, UINT, UINT, UINT)>(list, 12)(list, 3, 1, 0, 0);
  CHECK(slot<HRESULT(WINAPI*)(void*)>(list, 9)(list) == S_OK);

  const Recording* r = recording_of(list);
  CHECK(r && r->closed && r->commands.size() == 3);
  if (r && r->commands.size() == 3) {
    const Command& clear = r->commands[0];
    CHECK(clear.op == kClearRenderTargetView && clear.a[0] == 0x1234 && clear.data_size == 16);
    float kept[4] = {};
    std::memcpy(kept, r->data.data() + clear.data_at, sizeof kept);
    CHECK(kept[2] == 0.75f);
    const Command& rts = r->commands[1];
    uint64_t handles[3] = {};
    CHECK(rts.op == kSetRenderTargets && rts.data_size == 24);
    std::memcpy(handles, r->data.data() + rts.data_at, sizeof handles);
    CHECK(handles[1] == 0x140 && handles[2] == 0x200);
    CHECK(r->commands[2].op == kDrawInstanced && r->commands[2].a[0] == 3);
  }
  CHECK(slot<HRESULT(WINAPI*)(void*, void*, void*)>(list, 139)(list, nullptr, nullptr) == S_OK);
  CHECK(recording_of(list)->commands.empty());
}

TEST_CASE(d3d12x_clear_runs_on_the_host_gpu_and_presents) {
  using namespace am::rt;
  void* device = make_device();
  // A 64 x 48 render target (slot 127) and a view of it in a render-target heap of ours.
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 64;
  desc.Height = 48;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  using CreateFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*, UINT,
                                    const D3D12_CLEAR_VALUE*, void*, UINT, const DXGI_FORMAT*, const GUID*, void**);
  void* target = nullptr;
  CHECK(slot<CreateFn>(device, 127)(device, &heap, 0, &desc, 2, nullptr, nullptr, 0, nullptr, nullptr, &target) == S_OK);
  struct {
    uint32_t type, count, flags, node_mask;
    uint64_t address, size;
  } heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, 0, 1, 0, 0};
  void* rtv_heap = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 14)(device, &heap_desc, nullptr, &rtv_heap) == S_OK);
  const uint64_t rtv = slot<uint64_t(WINAPI*)(void*)>(rtv_heap, 9)(rtv_heap);
  slot<void(WINAPI*)(void*, void*, const void*, uint64_t)>(device, 20)(device, target, nullptr, rtv);

  // The console list clears it; the console queue submits it.
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  void* list = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const uint32_t*, void*, void*, const GUID*, void**)>(device, 73)(
            device, list_desc, nullptr, nullptr, nullptr, &list) == S_OK);
  const float color[4] = {1.0f, 0.5f, 0.0f, 1.0f};
  slot<void(WINAPI*)(void*, uint64_t, const float*, UINT, const D3D12_RECT*)>(list, 52)(list, rtv, color, 0, nullptr);
  CHECK(slot<HRESULT(WINAPI*)(void*)>(list, 9)(list) == S_OK);
  D3D12_COMMAND_QUEUE_DESC qd{};
  void* queue = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 8)(device, &qd, nullptr, &queue) == S_OK);
  slot<void(WINAPI*)(void*, UINT, void* const*, uint64_t, UINT)>(queue, 35)(queue, 1, &list, 0, 0);

  ID3D12Resource* host = host_texture(target, false);
  if (!present::host_gpu()) {
    CHECK(host == nullptr);  // no D3D12 here: nothing ran
    return;
  }
  CHECK(host != nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"AmericasClearTest";
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"clear test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr, nullptr,
                                wc.hInstance, nullptr);
  present::Presenter presenter(window);
  present::Image image;
  image.gpu = host;
  image.width = 64;
  image.height = 48;
  image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  std::vector<uint8_t> shown;
  CHECK(presenter.present(image, &shown));
  CHECK(shown.size() == 64u * 48 * 4);
  bool all = shown.size() == 64u * 48 * 4;
  for (size_t i = 0; all && i < shown.size(); i += 4) {
    all = shown[i] == 255 && (shown[i + 1] == 127 || shown[i + 1] == 128) && shown[i + 2] == 0 && shown[i + 3] == 255;
  }
  CHECK(all);
  DestroyWindow(window);
}

// The runtime binding: a translated program reads title memory through the page table and
// writes back into it, zero copy. Its user SGPRs are root constants: s[0:1] the input's
// address, s[2:3] the output's.
TEST_CASE(d3d12x_translated_program_uses_title_memory_in_place) {
  using namespace am;
  const std::vector<uint32_t> code = {
      0xf4000100, 0xfa000004,  // s_load_dword s4, s[0:1], 0x4
      0xbf8cc07f,              // s_waitcnt lgkmcnt(0)
      0x7e020204,              // v_mov_b32 v1, s4
      0x7e040280,              // v_mov_b32 v2, 0
      0xdc308008, 0x03000002,  // global_load_dword v3, v2, s[0:1] offset:8
      0xbf8c3f70,              // s_waitcnt vmcnt(0)
      0x4a020701,              // v_add_nc_u32 v1, v1, v3
      0xdc708000, 0x00020102,  // global_store_dword v2, v1, s[2:3]
      0xbf810000,              // s_endpgm
  };
  shader::TranslateInput in;
  in.code = code;
  in.stage = shader::HwStage::Cs;
  in.vgprs = 4;
  in.cs_rsrc2 = 4u << 1;  // four user SGPRs
  in.cs_threads[0] = 64;
  const shader::Translation t = shader::translate(in, shader::Binding::Runtime);
  CHECK(t.ok);
  if (!t.ok) {
    std::printf("    not translated: %s\n", t.failure.c_str());
    return;
  }
  std::string why;
  if (!rt::present::host_gpu() || !host::dxc_available(&why)) return;
  const host::CompileResult c = host::compile_hlsl(t.hlsl, t.entry, t.profile);
  CHECK(c.ok);
  if (!c.ok) {
    std::printf("    DXC: %s\n", c.messages.substr(0, 2000).c_str());
    return;
  }
  // Title memory: a 64 KiB allocation of its own in the title's range (below 2^40), as the game's
  // graphics heaps are.
  auto* mem = static_cast<uint32_t*>(rt::mem::allocate_title_range(65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  CHECK(mem != nullptr);
  if (!mem) return;
  mem[1] = 40;
  mem[2] = 2;
  CHECK(open_gpu_window(reinterpret_cast<uint64_t>(mem)));
  rt::present::HostGpu* gpu = rt::present::host_gpu();
  ID3D12RootSignature* root = translated_root_signature();
  CHECK(root != nullptr);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = root;
  pd.CS = {c.dxil.data(), c.dxil.size()};
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  CHECK(SUCCEEDED(gpu->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))));
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  gpu->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  gpu->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list));
  flush_page_table(list.Get());
  flush_bindings(list.Get());
  ID3D12DescriptorHeap* heaps[] = {translated_shader_heap(), sampler_heap()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetComputeRootSignature(root);
  list->SetComputeRootUnorderedAccessView(1, bindings_table());  // the image binding table, u0
  const uint64_t in_at = reinterpret_cast<uint64_t>(mem), out_at = in_at + 0x100;
  uint32_t user[kUserDataConstants] = {static_cast<uint32_t>(in_at), static_cast<uint32_t>(in_at >> 32),
                                       static_cast<uint32_t>(out_at), static_cast<uint32_t>(out_at >> 32)};
  list->SetComputeRoot32BitConstants(0, kUserDataConstants, user, 0);
  list->Dispatch(1, 1, 1);
  list->Close();
  ID3D12CommandList* lists[] = {list.Get()};
  gpu->wait(gpu->submit(1, lists));
  CHECK(SUCCEEDED(gpu->device->GetDeviceRemovedReason()));
  CHECK(mem[0x40] == 42);  // 40 + 2, written by the GPU into the title's own pages
  VirtualFree(mem, 0, MEM_RELEASE);
}

// What the driver's pipeline compiles cost the process: with AM_PIPELINE_MEMORY set to a folder
// of compiled programs (the shader service's dxil folder), the largest compute programs there
// (AM_PIPELINE_MEMORY_COUNT, default 12) are made into pipelines one at a time, and the process
// commit and the GPU's memory are logged around each. Nothing runs on the GPU. Without the
// variable the test does nothing.
TEST_CASE(d3d12x_driver_pipeline_memory) {
  using namespace am;
  char dir[1024] = {}, count_text[32] = {};
  if (!GetEnvironmentVariableA("AM_PIPELINE_MEMORY", dir, sizeof dir)) return;
  const size_t count = GetEnvironmentVariableA("AM_PIPELINE_MEMORY_COUNT", count_text, sizeof count_text) ? std::stoul(count_text) : 12;
  rt::present::HostGpu* gpu = rt::present::host_gpu();
  if (!gpu) return;
  ID3D12RootSignature* root = translated_root_signature();
  CHECK(root != nullptr);
  if (!root) return;
  // The program kind of a DXIL container: its DXIL part's version word, kind in the high half.
  const auto kind_of = [](const std::vector<uint8_t>& b) -> int {
    if (b.size() < 32 || std::memcmp(b.data(), "DXBC", 4) != 0) return -1;
    uint32_t parts = 0;
    std::memcpy(&parts, b.data() + 28, 4);
    for (uint32_t k = 0; k < parts && 32 + 4 * k + 4 <= b.size(); ++k) {
      uint32_t at = 0;
      std::memcpy(&at, b.data() + 32 + 4 * k, 4);
      if (at + 12 > b.size()) return -1;
      if (std::memcmp(b.data() + at, "DXIL", 4) == 0) {
        uint32_t version = 0;
        std::memcpy(&version, b.data() + at + 8, 4);
        return static_cast<int>(version >> 16);
      }
    }
    return -1;
  };
  std::vector<std::pair<uintmax_t, std::filesystem::path>> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".dxil") files.emplace_back(e.file_size(ec), e.path());
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  const auto commit = [] {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    return static_cast<double>(pm.PrivateUsage) / (1 << 20);
  };
  Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
  gpu->adapter.As(&a3);
  const auto video = [&](DXGI_MEMORY_SEGMENT_GROUP group) {
    DXGI_QUERY_VIDEO_MEMORY_INFO i{};
    if (a3) a3->QueryVideoMemoryInfo(0, group, &i);
    return static_cast<double>(i.CurrentUsage) / (1 << 20);
  };
  std::printf("    start: commit %.0f MiB, GPU local %.0f MiB, shared %.0f MiB\n", commit(), video(DXGI_MEMORY_SEGMENT_GROUP_LOCAL),
              video(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL));
  std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> made;
  for (const auto& [size, path] : files) {
    if (made.size() >= count) break;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (kind_of(b) != 5) continue;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.CS = {b.data(), b.size()};
    const double before = commit(), local_before = video(DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
    const auto t0 = std::chrono::steady_clock::now();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = gpu->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    %s: %.0f KiB of DXIL, 0x%08lx, %.0f ms, commit %+.1f MiB (now %.0f), GPU local %+.1f MiB\n",
                path.stem().string().substr(0, 16).c_str(), size / 1024.0, static_cast<unsigned long>(hr), ms, commit() - before, commit(),
                video(DXGI_MEMORY_SEGMENT_GROUP_LOCAL) - local_before);
    if (SUCCEEDED(hr)) made.push_back(pso);
  }
  std::printf("    %zu made: commit %.0f MiB, GPU local %.0f MiB\n", made.size(), commit(), video(DXGI_MEMORY_SEGMENT_GROUP_LOCAL));
  made.clear();
  std::printf("    released: commit %.0f MiB, GPU local %.0f MiB\n", commit(), video(DXGI_MEMORY_SEGMENT_GROUP_LOCAL));
}
