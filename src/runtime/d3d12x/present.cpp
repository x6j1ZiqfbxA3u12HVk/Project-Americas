// PresentX, the command queue's slot 32: (plane count, plane parameters, present parameters).
// The game presents one plane a frame. A plane's parameters, 72 bytes each:
//
//   +0x00 frame token (from WaitFrameEventX)    +0x08 resource count (1)
//   +0x10 resources                             +0x18 source rectangles (one per resource)
//   +0x20 destination placement                 +0x28 color space (DXGI_COLOR_SPACE_TYPE)
//
// The first resource of the first plane is shown in the game window (runtime/present). Until
// the GPU back end runs the title's commands, nothing draws into it: the window shows what
// the title's memory holds, black.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#include "core/text.h"
#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/layout.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"
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
  wchar_t text[160];
  std::swprintf(text, 160, L"%.0f fps, frame %llu%s", frames / seconds, static_cast<unsigned long long>(total),
                shown ? L" (no GPU rendering yet)" : L" (not shown: see run.log)");
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
      if (present::Presenter* p = presenter()) shown = p->present(image);
    }
  }
  count_frame(shown);
  return S_OK;
}

}  // namespace am::rt::d3d12x
