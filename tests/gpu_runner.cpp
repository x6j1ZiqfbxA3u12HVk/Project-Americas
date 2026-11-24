#include "gpu_runner.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace am::test {
namespace {

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12RootSignature> root;
  std::string why;
};

const Gpu& gpu() {
  static const Gpu g = [] {
    Gpu x;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&x.device)))) {
      x.why = "no D3D12 device";
      return x;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(x.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
      x.why = "the GPU lacks shader model 6.6";
      x.device.Reset();
      return x;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    x.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&x.queue));
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(x.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&x.root)))) {
      x.why = "cannot make the root signature";
      x.device.Reset();
    }
    return x;
  }();
  return g;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* d, size_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                              D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES hp{type};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> r;
  d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
  return r;
}

}  // namespace

GpuRun run_compute(const std::vector<uint8_t>& dxil, uint32_t groups, const std::vector<uint32_t>& args,
                   std::vector<uint32_t> memory, size_t dump_dwords) {
  GpuRun run;
  const Gpu& g = gpu();
  if (!g.device) {
    run.why = g.why;
    return run;
  }
  ID3D12Device* d = g.device.Get();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g.root.Get();
  pd.CS = {dxil.data(), dxil.size()};
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) {
    run.why = "the driver refused the pipeline";
    return run;
  }
  if (memory.empty()) memory.resize(1);
  const size_t mem_bytes = memory.size() * 4, dump_bytes = dump_dwords * 4;
  auto cb = buffer(d, 256 * ((args.size() * 4 + 255) / 256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto upload = buffer(d, mem_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto mem = buffer(d, mem_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto dump = buffer(d, dump_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(d, mem_bytes + dump_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  void* p = nullptr;
  cb->Map(0, nullptr, &p);
  std::memcpy(p, args.data(), args.size() * 4);
  cb->Unmap(0, nullptr);
  upload->Map(0, nullptr, &p);
  std::memcpy(p, memory.data(), mem_bytes);
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list));
  list->CopyBufferRegion(mem.Get(), 0, upload.Get(), 0, mem_bytes);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = mem.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  list->ResourceBarrier(1, &b);
  list->SetComputeRootSignature(g.root.Get());
  list->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, mem->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, dump->GetGPUVirtualAddress());
  list->Dispatch(groups, 1, 1);
  D3D12_RESOURCE_BARRIER bs[2] = {b, b};
  bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  bs[1] = bs[0];
  bs[1].Transition.pResource = dump.Get();
  list->ResourceBarrier(2, bs);
  list->CopyBufferRegion(back.Get(), 0, mem.Get(), 0, mem_bytes);
  list->CopyBufferRegion(back.Get(), mem_bytes, dump.Get(), 0, dump_bytes);
  list->Close();
  ID3D12CommandList* lists[] = {list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g.queue->Signal(fence.Get(), 1);
  fence->SetEventOnCompletion(1, done);
  WaitForSingleObject(done, 10000);
  CloseHandle(done);
  if (fence->GetCompletedValue() != 1 || FAILED(d->GetDeviceRemovedReason())) {
    run.why = "the GPU did not finish";
    return run;
  }
  back->Map(0, nullptr, &p);
  run.memory.resize(memory.size());
  run.dump.resize(dump_dwords);
  std::memcpy(run.memory.data(), p, mem_bytes);
  std::memcpy(run.dump.data(), static_cast<uint8_t*>(p) + mem_bytes, dump_bytes);
  back->Unmap(0, nullptr);
  run.ran = true;
  return run;
}

}  // namespace am::test
