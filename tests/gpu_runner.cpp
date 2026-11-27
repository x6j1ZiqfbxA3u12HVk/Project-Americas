#include "gpu_runner.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace am::test {
namespace {

// The debug layer's messages since the last call (with AM_D3D12_DEBUG=1), for a failure's reason.
std::string debug_messages(ID3D12Device* d) {
  ComPtr<ID3D12InfoQueue> q;
  if (FAILED(d->QueryInterface(IID_PPV_ARGS(&q)))) return {};
  std::string out;
  for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; ++i) {
    SIZE_T size = 0;
    q->GetMessage(i, nullptr, &size);
    std::vector<char> buffer(size);
    auto* m = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
    if (SUCCEEDED(q->GetMessage(i, m, &size))) out += std::string(": ") + m->pDescription;
  }
  q->ClearStoredMessages();
  return out;
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12RootSignature> root, graphics_root;  // the second also takes an input layout
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
    // AM_D3D12_DEBUG=1: the debug layer, whose messages say why the driver refuses a pipeline.
    if (GetEnvironmentVariableA("AM_D3D12_DEBUG", nullptr, 0)) {
      ComPtr<ID3D12Debug> debug;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&x.device)))) {
      x.why = "no D3D12 device";
      return x;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (FAILED(x.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6 ||
        FAILED(x.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)) ||
        o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
      x.why = "the GPU lacks shader model 6.6 or resource binding tier 3";
      x.device.Reset();
      return x;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    x.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&x.queue));
    D3D12_ROOT_PARAMETER1 params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 1; i < 4; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      params[i].Descriptor.ShaderRegister = i - 1;
      params[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{};
    rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rd.Desc_1_1 = {4, params, 0, nullptr,
                   D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED};
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rd, &blob, &errors)) ||
        FAILED(x.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&x.root)))) {
      x.why = "cannot make the root signature";
      x.device.Reset();
      return x;
    }
    rd.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rd, &blob, &errors)) ||
        FAILED(x.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&x.graphics_root)))) {
      x.why = "cannot make the graphics root signature";
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
  desc.Width = std::max<size_t>(size, 4);
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

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

struct Texture {
  ComPtr<ID3D12Resource> resource, upload;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};

// Makes the texture, records its upload, and writes its views into the heap.
bool make_texture(ID3D12Device* d, ID3D12GraphicsCommandList* list, const TextureSpec& t, D3D12_CPU_DESCRIPTOR_HANDLE heap,
                  UINT step, size_t index, Texture& out) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = t.kind == TextureSpec::Tex1D   ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                   : t.kind == TextureSpec::Tex3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                  : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = t.width;
  desc.Height = t.kind == TextureSpec::Tex1D ? 1 : t.height;
  desc.DepthOrArraySize = static_cast<UINT16>(t.depth_or_layers);
  desc.MipLevels = static_cast<UINT16>(t.mips);
  desc.Format = t.format;
  desc.SampleDesc.Count = 1;
  desc.Flags = t.uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
  if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&out.resource)))) {
    return false;
  }
  const UINT layers = t.kind == TextureSpec::Tex3D ? 1 : t.depth_or_layers;
  const UINT subs = t.mips * layers;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
  std::vector<UINT> rows(subs);
  std::vector<UINT64> row_bytes(subs);
  UINT64 total = 0;
  d->GetCopyableFootprints(&desc, 0, subs, 0, fp.data(), rows.data(), row_bytes.data(), &total);
  out.upload = buffer(d, static_cast<size_t>(total), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t* p = nullptr;
  out.upload->Map(0, nullptr, reinterpret_cast<void**>(&p));
  std::memset(p, 0, static_cast<size_t>(total));
  for (UINT s = 0; s < subs && s < t.data.size(); ++s) {
    const UINT depth = fp[s].Footprint.Depth;
    for (UINT z = 0; z < depth; ++z) {
      for (UINT y = 0; y < rows[s]; ++y) {
        const size_t from = (static_cast<size_t>(z) * rows[s] + y) * row_bytes[s];
        if (from + row_bytes[s] > t.data[s].size()) break;
        std::memcpy(p + fp[s].Offset + (static_cast<size_t>(z) * rows[s] + y) * fp[s].Footprint.RowPitch, t.data[s].data() + from,
                    static_cast<size_t>(row_bytes[s]));
      }
    }
  }
  out.upload->Unmap(0, nullptr);
  for (UINT s = 0; s < subs; ++s) {
    D3D12_TEXTURE_COPY_LOCATION dst{out.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = s;
    D3D12_TEXTURE_COPY_LOCATION src{out.upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp[s];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  out.state = t.uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const D3D12_RESOURCE_BARRIER b = transition(out.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, out.state);
  list->ResourceBarrier(1, &b);

  // The views.
  const auto at = [&](size_t k) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap;
    h.ptr += (3 * index + k) * step;
    return h;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = t.format;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
  uv.Format = t.format;
  switch (t.kind) {
    case TextureSpec::Tex1D:
      if (t.array) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        sv.Texture1DArray = {0, t.mips, 0, t.depth_or_layers, 0.0f};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        uv.Texture1DArray = {0, 0, t.depth_or_layers};
      } else {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        sv.Texture1D = {0, t.mips, 0.0f};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
      }
      break;
    case TextureSpec::Tex2D:
      if (t.array) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray = {0, t.mips, 0, t.depth_or_layers, 0, 0.0f};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uv.Texture2DArray = {0, 0, t.depth_or_layers, 0};
      } else {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D = {0, t.mips, 0, 0.0f};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      }
      break;
    case TextureSpec::Tex3D:
      sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      sv.Texture3D = {0, t.mips, 0.0f};
      uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
      uv.Texture3D = {0, 0, t.depth_or_layers};
      break;
    case TextureSpec::Cube:
      if (t.array) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        sv.TextureCubeArray = {0, t.mips, 0, t.depth_or_layers / 6, 0.0f};
      } else {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube = {0, t.mips, 0.0f};
      }
      uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
      uv.Texture2DArray = {0, 0, t.depth_or_layers, 0};
      break;
  }
  d->CreateShaderResourceView(out.resource.Get(), &sv, at(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC alt = sv;
  alt.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  alt.Texture2DArray = {0, t.mips, 0, t.depth_or_layers, 0, 0.0f};
  if (t.kind == TextureSpec::Cube) d->CreateShaderResourceView(out.resource.Get(), &alt, at(1));
  if (t.uav) d->CreateUnorderedAccessView(out.resource.Get(), nullptr, &uv, at(2));
  return true;
}

}  // namespace

GpuRun run_compute(const std::vector<uint8_t>& dxil, uint32_t groups, const std::vector<uint32_t>& args,
                   std::vector<uint32_t> memory, size_t dump_dwords, const ImageSetup* images) {
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
  if (const HRESULT hr = d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)); FAILED(hr)) {
    char code[48];
    std::snprintf(code, sizeof code, " (0x%08lX)", static_cast<unsigned long>(hr));
    run.why = "the driver refused the pipeline" + std::string(code) + debug_messages(d);
    return run;
  }
  if (memory.empty()) memory.resize(1);
  // Without images, a table with nothing in it: every lookup misses, nothing is queued.
  std::vector<uint32_t> bindings = images && !images->bindings.empty() ? images->bindings : std::vector<uint32_t>(8, 0);
  const size_t mem_bytes = memory.size() * 4, dump_bytes = dump_dwords * 4, bind_bytes = bindings.size() * 4;
  auto cb = buffer(d, 256 * ((args.size() * 4 + 255) / 256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto upload = buffer(d, mem_bytes + bind_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto mem = buffer(d, mem_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto bind = buffer(d, bind_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto dump = buffer(d, dump_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(d, mem_bytes + dump_bytes + bind_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  void* p = nullptr;
  cb->Map(0, nullptr, &p);
  std::memcpy(p, args.data(), args.size() * 4);
  cb->Unmap(0, nullptr);
  upload->Map(0, nullptr, &p);
  std::memcpy(p, memory.data(), mem_bytes);
  std::memcpy(static_cast<uint8_t*>(p) + mem_bytes, bindings.data(), bind_bytes);
  upload->Unmap(0, nullptr);

  // Descriptor heaps: three views a texture, one slot a sampler.
  const size_t ntex = images ? images->textures.size() : 0, nsamp = images ? images->samplers.size() : 0;
  ComPtr<ID3D12DescriptorHeap> views, samplers;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, static_cast<UINT>(std::max<size_t>(3 * ntex, 1)),
                                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views));
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, static_cast<UINT>(std::max<size_t>(nsamp, 1)), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&samplers));
  const UINT vstep = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT sstep = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (size_t j = 0; j < nsamp; ++j) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = samplers->GetCPUDescriptorHandleForHeapStart();
    h.ptr += j * sstep;
    d->CreateSampler(&images->samplers[j], h);
  }

  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list));
  std::vector<Texture> textures(ntex);
  for (size_t i = 0; i < ntex; ++i) {
    if (!make_texture(d, list.Get(), images->textures[i], views->GetCPUDescriptorHandleForHeapStart(), vstep, i, textures[i])) {
      run.why = "cannot make test texture " + std::to_string(i);
      return run;
    }
  }
  list->CopyBufferRegion(mem.Get(), 0, upload.Get(), 0, mem_bytes);
  list->CopyBufferRegion(bind.Get(), 0, upload.Get(), mem_bytes, bind_bytes);
  D3D12_RESOURCE_BARRIER in[2] = {transition(mem.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                  transition(bind.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
  list->ResourceBarrier(2, in);
  ID3D12DescriptorHeap* heaps[] = {views.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);  // before the root signature, which indexes them directly
  list->SetComputeRootSignature(g.root.Get());
  list->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, mem->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, dump->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(3, bind->GetGPUVirtualAddress());
  list->Dispatch(groups, 1, 1);
  D3D12_RESOURCE_BARRIER out[3] = {transition(mem.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                   transition(dump.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                   transition(bind.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  list->ResourceBarrier(3, out);
  list->CopyBufferRegion(back.Get(), 0, mem.Get(), 0, mem_bytes);
  list->CopyBufferRegion(back.Get(), mem_bytes, dump.Get(), 0, dump_bytes);
  list->CopyBufferRegion(back.Get(), mem_bytes + dump_bytes, bind.Get(), 0, bind_bytes);
  // Each texture's first subresource, for the tests that write them.
  std::vector<ComPtr<ID3D12Resource>> tex_back(ntex);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> tex_fp(ntex);
  std::vector<UINT> tex_rows(ntex);
  std::vector<UINT64> tex_row_bytes(ntex);
  for (size_t i = 0; i < ntex; ++i) {
    const D3D12_RESOURCE_DESC desc = textures[i].resource->GetDesc();
    UINT64 total = 0;
    d->GetCopyableFootprints(&desc, 0, 1, 0, &tex_fp[i], &tex_rows[i], &tex_row_bytes[i], &total);
    tex_back[i] = buffer(d, static_cast<size_t>(total), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_RESOURCE_BARRIER b = transition(textures[i].resource.Get(), textures[i].state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{textures[i].resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{tex_back[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = tex_fp[i];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
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
  run.bindings.resize(bindings.size());
  std::memcpy(run.memory.data(), p, mem_bytes);
  std::memcpy(run.dump.data(), static_cast<uint8_t*>(p) + mem_bytes, dump_bytes);
  std::memcpy(run.bindings.data(), static_cast<uint8_t*>(p) + mem_bytes + dump_bytes, bind_bytes);
  back->Unmap(0, nullptr);
  run.textures.resize(ntex);
  for (size_t i = 0; i < ntex; ++i) {
    uint8_t* q = nullptr;
    tex_back[i]->Map(0, nullptr, reinterpret_cast<void**>(&q));
    const size_t rb = static_cast<size_t>(tex_row_bytes[i]), depth = tex_fp[i].Footprint.Depth;
    run.textures[i].resize(rb * tex_rows[i] * depth);
    for (size_t z = 0; z < depth; ++z) {
      for (size_t y = 0; y < tex_rows[i]; ++y) {
        std::memcpy(run.textures[i].data() + (z * tex_rows[i] + y) * rb, q + tex_fp[i].Offset + (z * tex_rows[i] + y) * tex_fp[i].Footprint.RowPitch, rb);
      }
    }
    tex_back[i]->Unmap(0, nullptr);
  }
  run.ran = true;
  return run;
}

GraphicsRun run_graphics(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps,
                         const std::vector<D3D12_INPUT_ELEMENT_DESC>& layout, const std::vector<float>& vertices,
                         uint32_t stride, uint32_t vertex_count, uint32_t width, uint32_t height,
                         const std::vector<uint32_t>& args) {
  GraphicsRun run;
  const Gpu& g = gpu();
  if (!g.device) {
    run.why = g.why;
    return run;
  }
  ID3D12Device* d = g.device.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g.graphics_root.Get();
  pd.VS = {vs.data(), vs.size()};
  pd.PS = {ps.data(), ps.size()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pd.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
    run.why = "the driver refused the pipeline" + debug_messages(d);
    return run;
  }
  const size_t vb_bytes = vertices.size() * sizeof(float);
  auto vb = buffer(d, vb_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto cb = buffer(d, 256 * ((args.size() * 4 + 255) / 256 + 1), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto scratch = buffer(d, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  void* p = nullptr;
  vb->Map(0, nullptr, &p);
  std::memcpy(p, vertices.data(), vb_bytes);
  vb->Unmap(0, nullptr);
  cb->Map(0, nullptr, &p);
  std::memset(p, 0, 256);
  std::memcpy(p, args.data(), args.size() * 4);
  cb->Unmap(0, nullptr);

  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = width;
  td.Height = height;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> target;
  if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&target)))) {
    run.why = "no render target";
    return run;
  }
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs));
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  d->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 row_bytes = 0, total = 0;
  d->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total);
  auto back = buffer(d, static_cast<size_t>(total), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list));
  const float clear[4] = {0, 0, 0, 0};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(g.graphics_root.Get());
  list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
  for (UINT k = 1; k < 4; ++k) list->SetGraphicsRootUnorderedAccessView(k, scratch->GetGPUVirtualAddress());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), static_cast<UINT>(vb_bytes), stride};
  list->IASetVertexBuffers(0, 1, &view);
  list->DrawInstanced(vertex_count, 1, 0, 0);
  const D3D12_RESOURCE_BARRIER b = transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION dst{back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = fp;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
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
  uint8_t* q = nullptr;
  back->Map(0, nullptr, reinterpret_cast<void**>(&q));
  run.pixels.resize(size_t{width} * height * 4);
  for (UINT y = 0; y < height; ++y) {
    std::memcpy(run.pixels.data() + size_t{y} * width * 4, q + fp.Offset + size_t{y} * fp.Footprint.RowPitch, size_t{width} * 4);
  }
  back->Unmap(0, nullptr);
  run.ran = true;
  return run;
}

}  // namespace am::test
