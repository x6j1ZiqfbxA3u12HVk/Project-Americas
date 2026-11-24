#pragma once

// A small D3D12 compute harness for the translator's tests: runs one compiled program with the
// tests' binding model (constant buffer b0, memory u0, register dump u1, image bindings u2,
// descriptor heaps indexed directly) on the host GPU.

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace am::test {

// A texture for a test. Its views sit in the descriptor heap at 3 i (SRV: the texture's own
// type), 3 i + 1 (a 2D-array SRV of a cube's faces) and 3 i + 2 (UAV, when `uav`).
struct TextureSpec {
  enum Kind : uint8_t { Tex1D, Tex2D, Tex3D, Cube } kind = Tex2D;
  DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  uint32_t width = 1, height = 1, depth_or_layers = 1, mips = 1;
  bool array = false;  // array views (Texture2DArray, TextureCubeArray, ...)
  bool uav = false;    // writable: kept in the UAV state, so loads go through the UAV
  // Each subresource's texels, rows packed tight; D3D's order (mip + layer * mips).
  std::vector<std::vector<uint8_t>> data;
};

struct ImageSetup {
  std::vector<TextureSpec> textures;
  std::vector<D3D12_SAMPLER_DESC> samplers;  // sampler j at heap index j
  std::vector<uint32_t> bindings;            // the am_bindings buffer (shader/image_bindings.h)
};

struct GpuRun {
  bool ran = false;
  std::string why;  // when it did not: no GPU, no SM 6.6, a failure
  std::vector<uint32_t> memory;
  std::vector<uint32_t> dump;
  std::vector<uint32_t> bindings;               // read back: the miss list
  std::vector<std::vector<uint8_t>> textures;   // each texture's subresource 0, rows packed tight
};

// `args` is the constant buffer (32 user SGPRs, then the memory's GPU base address).
GpuRun run_compute(const std::vector<uint8_t>& dxil, uint32_t groups, const std::vector<uint32_t>& args,
                   std::vector<uint32_t> memory, size_t dump_dwords, const ImageSetup* images = nullptr);

}  // namespace am::test
