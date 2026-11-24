// The presenter: an image from memory, through the host's D3D12 into a swap chain, read back.
// On a machine without a D3D12 GPU the presenter declines and the test checks only that.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "runtime/present/presenter.h"
#include "test.h"

using namespace am::rt::present;

namespace {

HWND hidden_window() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"AmericasPresentTest";
  RegisterClassW(&wc);
  return CreateWindowExW(0, wc.lpszClassName, L"present test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr, nullptr,
                         wc.hInstance, nullptr);
}

}  // namespace

TEST_CASE(present_formats) {
  CHECK(swap_chain_format(28) == DXGI_FORMAT_R8G8B8A8_UNORM);  // R8G8B8A8_UNORM, the game's back buffers
  CHECK(swap_chain_format(29) == DXGI_FORMAT_R8G8B8A8_UNORM);  // its sRGB view: the same bytes
  CHECK(swap_chain_format(91) == DXGI_FORMAT_B8G8R8A8_UNORM);
  CHECK(swap_chain_format(24) == DXGI_FORMAT_R10G10B10A2_UNORM);
  CHECK(swap_chain_format(10) == DXGI_FORMAT_R16G16B16A16_FLOAT);
  CHECK(swap_chain_format(61) == DXGI_FORMAT_UNKNOWN);  // R8_UNORM: no swap chain takes it
  CHECK(swap_chain_pixel_bytes(DXGI_FORMAT_R16G16B16A16_FLOAT) == 8);
}

TEST_CASE(present_image_reaches_the_swap_chain) {
  HWND window = hidden_window();
  CHECK(window != nullptr);
  Presenter presenter(window);
  // 70 x 33 RGBA8 in rows of 512 bytes (a padded pitch, as in the console layout).
  constexpr uint32_t w = 70, h = 33, pitch = 512;
  std::vector<uint8_t> memory(pitch * h, 0xEE);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* p = memory.data() + y * pitch + x * 4;
      p[0] = static_cast<uint8_t>(x * 3);
      p[1] = static_cast<uint8_t>(y * 7);
      p[2] = static_cast<uint8_t>(x + y);
      p[3] = 255;
    }
  }
  Image image;
  image.memory = memory.data();
  image.width = w;
  image.height = h;
  image.row_pitch = pitch;
  image.format = 28;
  std::vector<uint8_t> shown;
  const bool ok = presenter.present(image, &shown);
  if (!presenter.usable()) {
    CHECK(!ok);  // no D3D12 here: nothing to compare
  } else {
    CHECK(ok);
    CHECK(shown.size() == size_t{w} * h * 4);
    bool same = shown.size() == size_t{w} * h * 4;
    for (uint32_t y = 0; same && y < h; ++y) {
      same = std::memcmp(shown.data() + y * w * 4, memory.data() + y * pitch, w * 4) == 0;
    }
    CHECK(same);
    // A second frame through the other buffer slot.
    memory[0] = 0x12;
    CHECK(presenter.present(image, &shown) && shown[0] == 0x12);
  }
  DestroyWindow(window);
}
