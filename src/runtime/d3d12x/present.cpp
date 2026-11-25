// PresentX, the command queue's slot 32: (plane count, plane parameters, present parameters).
// The game presents one plane a frame. A plane's parameters, 72 bytes each:
//
//   +0x00 frame token (from WaitFrameEventX)    +0x08 resource count (1)
//   +0x10 resources                             +0x18 source rectangles (one per resource)
//   +0x20 destination placement                 +0x28 color space (DXGI_COLOR_SPACE_TYPE)
//
// The first resource of the first plane is shown in the game window (runtime/present): from its
// host texture when the back end has one (GPU to GPU), else from the title's memory.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <string>

#include "core/text.h"
#include "runtime/d3d12x/commands.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/host.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"
#include "runtime/process.h"
#include "runtime/present/presenter.h"
#include "runtime/present/window.h"

namespace am::rt::d3d12x {
namespace {

struct PlaneParameters {
  uint64_t token;
  uint32_t resource_count;
  uint32_t reserved0;
  Object* const* resources;
  const D3D12_RECT* source_rects;
  const void* destination;
  uint32_t color_space;
  uint32_t reserved1[7];
};
static_assert(sizeof(PlaneParameters) == 0x48);

present::Presenter* presenter() {
  static present::Presenter* const p = [] {
    HWND window = present::game_window();
    return window ? new present::Presenter(window) : nullptr;
  }();
  return p;
}

// The frame rate in the title bar, once a second.
// AMERICAS_CAPTURE lists frame numbers (as "300,1500"): those frames are saved, as shown, to
// <logs>\frame-<n>.bmp. For looking at what the back end draws without watching the window.
bool capture_wanted(uint64_t frame) {
  static const std::vector<uint64_t> frames = [] {
    std::vector<uint64_t> f;
    char* text = nullptr;
    size_t size = 0;
    if (_dupenv_s(&text, &size, "AMERICAS_CAPTURE") == 0 && text) {
      for (char* p = text; *p;) {
        char* end = nullptr;
        const unsigned long long n = std::strtoull(p, &end, 10);
        if (end == p) break;
        f.push_back(n);
        p = *end ? end + 1 : end;
      }
      std::free(text);
    }
    return f;
  }();
  return std::find(frames.begin(), frames.end(), frame) != frames.end();
}

// Rows of 8-bit RGBA or BGRA (as the swap chain holds them) as a 24-bit BMP, bottom row first.
void save_bmp(uint64_t frame, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, bool rgba) {
  TitleProcess* p = process();
  if (!p || p->logs_dir().empty() || pixels.size() < uint64_t{width} * height * 4) return;
  const uint32_t row = (width * 3 + 3) & ~3u, image = row * height;
  uint8_t header[54] = {'B', 'M'};
  const auto put = [&](size_t at, uint32_t v) { std::memcpy(header + at, &v, 4); };
  put(2, 54 + image);
  put(10, 54);
  put(14, 40);
  put(18, width);
  put(22, height);
  header[26] = 1;
  header[28] = 24;
  put(34, image);
  std::vector<uint8_t> out(image, 0);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* from = pixels.data() + uint64_t{height - 1 - y} * width * 4;
    uint8_t* to = out.data() + uint64_t{y} * row;
    for (uint32_t x = 0; x < width; ++x, from += 4, to += 3) {
      to[0] = rgba ? from[2] : from[0];
      to[1] = from[1];
      to[2] = rgba ? from[0] : from[2];
    }
  }
  const std::filesystem::path file = p->logs_dir() / ("frame-" + std::to_string(frame) + ".bmp");
  std::ofstream f(file, std::ios::binary);
  f.write(reinterpret_cast<const char*>(header), sizeof header);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  logf("present: frame %llu saved to %s", static_cast<unsigned long long>(frame), file.string().c_str());
}

void count_frame(bool shown) {
  static LARGE_INTEGER frequency{}, since{};
  static uint64_t frames = 0, total = 0;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!frequency.QuadPart) {
    QueryPerformanceFrequency(&frequency);
    since = now;
  }
  ++frames;
  ++total;
  const double seconds = static_cast<double>(now.QuadPart - since.QuadPart) / static_cast<double>(frequency.QuadPart);
  if (seconds < 1.0) return;
  wchar_t text[256];
  std::swprintf(text, 256, L"%.0f fps, frame %llu%s   |   A Enter, B Esc, Menu Tab, d-pad arrows, sticks WASD / IJKL",
                frames / seconds, static_cast<unsigned long long>(total), shown ? L"" : L" (not shown: see run.log)");
  present::set_window_status(text);
  frames = 0;
  since = now;
}

}  // namespace

HRESULT WINAPI queue_present_x(Object* self, UINT plane_count, const void* planes_in, const void* /*parameters*/) {
  const auto* planes = static_cast<const PlaneParameters*>(planes_in);
  trace(self, 32, "PresentX", plane_count, reinterpret_cast<uint64_t>(planes_in), 0);
  bool shown = false;
  if (plane_count && planes && planes[0].resource_count && planes[0].resources) {
    const PlaneParameters& plane = planes[0];
    D3D12_RESOURCE_DESC desc{};
    uint64_t address = 0;
    TextureLayout layout;
    if (resource_memory(plane.resources[0], &desc, &address) && texture_layout(desc, layout) &&
        !layout.subresources.empty()) {
      const SubresourceLayout& top = layout.subresources[0];
      uint32_t left = 0, upper = 0, width = static_cast<uint32_t>(desc.Width), height = desc.Height;
      if (const D3D12_RECT* r = plane.source_rects; r && r->right > r->left && r->bottom > r->top) {
        left = static_cast<uint32_t>(std::max<LONG>(r->left, 0));
        upper = static_cast<uint32_t>(std::max<LONG>(r->top, 0));
        width = std::min<uint32_t>(static_cast<uint32_t>(r->right) - left, width - std::min(left, width));
        height = std::min<uint32_t>(static_cast<uint32_t>(r->bottom) - upper, height - std::min(upper, height));
      }
      present::Image image;
      // A target the back end draws into is on the GPU: shown from there. Else the title's memory.
      image.gpu = host_texture(plane.resources[0], false);
      image.left = left;
      image.top = upper;
      image.memory = reinterpret_cast<const uint8_t*>(address) + top.offset + uint64_t{upper} * top.row_pitch +
                     uint64_t{left} * layout.element_bytes;
      image.width = width;
      image.height = height;
      image.row_pitch = top.row_pitch;
      image.format = desc.Format;
      image.color_space = plane.color_space;
      static std::atomic<bool> logged{false};
      if (!logged.exchange(true)) {
        logf("d3d12x: PresentX(%u planes): %ux%u of a %llux%u format %u resource at 0x%s, color space %u", plane_count,
             width, height, static_cast<unsigned long long>(desc.Width), desc.Height, desc.Format, hex(address).c_str(),
             plane.color_space);
      }
      static std::atomic<uint64_t> presented{0};
      const uint64_t frame = ++presented;
      const DXGI_FORMAT shown_as = present::swap_chain_format(image.format);
      const bool capture = capture_wanted(frame) && (shown_as == DXGI_FORMAT_R8G8B8A8_UNORM || shown_as == DXGI_FORMAT_B8G8R8A8_UNORM);
      std::vector<uint8_t> pixels;
      if (present::Presenter* p = presenter()) shown = p->present(image, capture ? &pixels : nullptr);
      if (capture && shown) save_bmp(frame, pixels, width, height, shown_as == DXGI_FORMAT_R8G8B8A8_UNORM);
    }
  }
  count_frame(shown);
  frame_present();
  return S_OK;
}

}  // namespace am::rt::d3d12x
