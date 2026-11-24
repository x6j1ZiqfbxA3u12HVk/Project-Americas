// The console graphics objects, called through their vtables the way title code calls them.

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstring>
#include <vector>

#include "runtime/d3d12x/d3d12x.h"
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
  bc.Format = DXGI_FORMAT_BC1_UNORM;  // 64 x 64 blocks of 8 bytes
  bc.SampleDesc.Count = 1;
  slot<InfoFn>(device, 25)(&info, device, 1, 1, &bc);
  CHECK(info.SizeInBytes == 65536);
}

TEST_CASE(d3d12x_queue_signals_fence_at_once) {
  void* device = make_device();
  D3D12_COMMAND_QUEUE_DESC qd{};
  void* queue = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, const void*, const GUID*, void**)>(device, 8)(device, &qd, nullptr, &queue) == S_OK);
  void* fence = nullptr;
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64, UINT, const GUID*, void**)>(device, 36)(device, 0, 0, nullptr, &fence) == S_OK);
  CHECK(slot<HRESULT(WINAPI*)(void*, void*, UINT64)>(queue, 14)(queue, fence, 5) == S_OK);
  CHECK(slot<UINT64(WINAPI*)(void*)>(fence, 8)(fence) == 5);
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64, HANDLE)>(fence, 9)(fence, 7, event) == S_OK);
  CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT);  // not reached yet
  CHECK(slot<HRESULT(WINAPI*)(void*, UINT64)>(fence, 10)(fence, 7) == S_OK);
  CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
  CloseHandle(event);
  CHECK(slot<ULONG(WINAPI*)(void*)>(fence, 2)(fence) == 0);  // released and freed
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
