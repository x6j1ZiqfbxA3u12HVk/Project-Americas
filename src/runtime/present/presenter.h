#pragma once

// Shows the title's presented images in the game window, through the host's D3D12: a flip-model
// swap chain the size of the image, scaled to the window by DXGI. Each frame the image is copied
// from the title's memory into an upload buffer and from there into the swap chain's buffer.
//
// Any D3D12 GPU will do (the default high-performance adapter); without one, frames are counted
// but not shown.

#include <windows.h>
#include <dxgiformat.h>

#include <cstdint>
#include <vector>

namespace am::rt::present {

// One presented image in the title's memory: rows of `row_pitch` bytes.
struct Image {
  const uint8_t* memory = nullptr;
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
  // For `window`; the device is made on the first present.
  explicit Presenter(HWND window);
  ~Presenter();
  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;

  // Shows `image`. With `capture`, also returns the swap chain buffer's bytes as shown (rows of
  // width * pixel bytes; tests). False when it could not be shown.
  bool present(const Image& image, std::vector<uint8_t>* capture = nullptr);
  bool usable() const;  // false once the host could not give a device

 private:
  struct State;
  State* s_;
};

}  // namespace am::rt::present
