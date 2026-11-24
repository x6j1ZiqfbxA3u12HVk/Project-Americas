// d3d12_xs.dll: Direct3D 12.X. D3D12ConfigureVirtualMemoryX here; the device and its objects in
// runtime/d3d12x.

#include <windows.h>

#include <cstdint>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/hle/registry.h"
#include "runtime/log.h"

namespace am::rt::hle {
namespace {

// The game passes a zeroed 32-byte block whose word at +4 it derives from the size of the GPU
// address range it reserves next (one more for each 16 GiB, plus two). On the console this sizes
// the GPU's page tables; our graphics layer translates addresses its own way and needs nothing
// here, so the block is logged and accepted.
HRESULT WINAPI my_D3D12ConfigureVirtualMemoryX(const uint32_t* config) {
  if (!config) return E_INVALIDARG;
  logf("D3D12ConfigureVirtualMemoryX: %08x %08x %08x %08x %08x %08x %08x %08x", config[0], config[1], config[2],
       config[3], config[4], config[5], config[6], config[7]);
  return S_OK;
}

}  // namespace

void add_graphics(std::vector<Function>& out) {
  out.push_back(Function{"d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX",
                         reinterpret_cast<void*>(&my_D3D12ConfigureVirtualMemoryX)});
  out.push_back(Function{"d3d12_xs.dll", "D3D12XboxCreateDevice", reinterpret_cast<void*>(&d3d12x::create_device)});
}

}  // namespace am::rt::hle
