#pragma once

// Shows the title's presented images in the game window, through the host's D3D12 (host_gpu.h):
// a flip-model swap chain the size of the image, scaled to the window by DXGI.
//
// An image that lives on the GPU (a render target the back end draws into) is copied GPU to GPU.
// One that only exists in the title's memory is copied there through an upload buffer.

#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <vector>

namespace am::rt::present {

// One presented image. `gpu` set: a host resource, in the COMMON state between command lists.
// Else `memory`: rows of `row_pitch` bytes in the title's memory.
struct Image {
  ID3D12Resource* gpu = nullptr;
  const uint8_t* memory = nullptr;
  uint32_t left = 0, top = 0;  // the region shown, in the resource
  uint32_t width = 0, height = 0;
  uint32_t row_pitch = 0;
  uint32_t format = 0;       // the resource's DXGI format
  uint32_t color_space = 0;  // DXGI_COLOR_SPACE_TYPE, as the title gives it
};

// The swap chain format an image of `format` is shown in (DXGI_FORMAT_UNKNOWN: none).
DXGI_FORMAT swap_chain_format(uint32_t format);
// Bytes per pixel of the formats the swap chain takes (0 for others).
uint32_t swap_chain_pixel_bytes(DXGI_FORMAT format);

class Presenter {
 public:
  explicit Presenter(HWND window);
  ~Presenter();
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  // Shows `image`. With `capture`, also returns the swap chain buffer's bytes as shown (rows of
  // width * pixel bytes; tests). False when it could not be shown.
  bool present(const Image& image, std::vector<uint8_t>* capture = nullptr);
  bool usable() const;  // false when the host has no D3D12 device

 private:
  struct State;
  State* s_;
};

}  // namespace am::rt::present
