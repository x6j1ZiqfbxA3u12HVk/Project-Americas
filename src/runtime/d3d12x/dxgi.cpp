// The console device is also a DXGI device: the game asks it for IDXGIDevice, then for the
// adapter and its output (display modes, vertical blank). Slots follow DXGI's interfaces
// (IDXGIObject, IDXGIDevice / IDXGIDevice1, IDXGIAdapter, IDXGIOutput), which the game's calls
// match so far; the rest stay probes.
//
// The output reports the host's main display at 60 Hz: the refresh rate the console's titles
// are paced for. Frames reach the host's display through our own presentation later.

#include <windows.h>
#include <dxgi1_2.h>

#include <cstring>

#include "runtime/d3d12x/d3d12x.h"
#include "runtime/d3d12x/object.h"
#include "runtime/log.h"

namespace am::rt::d3d12x {
namespace {

constexpr GUID kIDXGIObject = {0xaec22fb8, 0x76f3, 0x4639, {0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e}};
constexpr GUID kIDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
constexpr GUID kIDXGIDevice1 = {0x77db970f, 0x6276, 0x48ba, {0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c}};
constexpr GUID kIDXGIDevice2 = {0x05008617, 0xfbfd, 0x4051, {0xa7, 0x90, 0x14, 0x48, 0x84, 0xb4, 0xf6, 0xa9}};
constexpr GUID kIDXGIDevice3 = {0x6007896c, 0x3244, 0x4afd, {0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23}};

struct Output : Object {
  UINT width = 1920, height = 1080;
};
struct Adapter : Object {
  Output* output = nullptr;
};
struct DxgiDevice : Object {
  Object* device = nullptr;
  Adapter* adapter = nullptr;
  INT gpu_priority = 0;
  UINT frame_latency = 3;
};

// ---- output (IDXGIOutput) ----

HRESULT WINAPI output_get_desc(Output* self, DXGI_OUTPUT_DESC* desc) {
  trace(self, 7, "GetDesc", reinterpret_cast<uint64_t>(desc), 0, 0);
  if (!desc) return E_INVALIDARG;
  *desc = {};
  wcscpy_s(desc->DeviceName, L"\\\\.\\DISPLAY1");
  desc->DesktopCoordinates = {0, 0, static_cast<LONG>(self->width), static_cast<LONG>(self->height)};
  desc->AttachedToDesktop = TRUE;
  desc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
  return S_OK;
}

void fill_mode(DXGI_MODE_DESC& m, UINT w, UINT h, DXGI_FORMAT format) {
  m = {};
  m.Width = w;
  m.Height = h;
  m.RefreshRate = {60, 1};
  m.Format = format;
  m.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
  m.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
}

HRESULT WINAPI output_get_display_mode_list(Output* self, DXGI_FORMAT format, UINT flags, UINT* count,
                                            DXGI_MODE_DESC* modes) {
  trace(self, 8, "GetDisplayModeList", static_cast<uint64_t>(format), flags, reinterpret_cast<uint64_t>(modes));
  if (!count) return E_INVALIDARG;
  if (!modes) {
    *count = 1;
    return S_OK;
  }
  if (*count < 1) return DXGI_ERROR_MORE_DATA;
  fill_mode(modes[0], self->width, self->height, format);
  *count = 1;
  return S_OK;
}

HRESULT WINAPI output_find_closest_matching_mode(Output* self, const DXGI_MODE_DESC* wanted, DXGI_MODE_DESC* closest,
                                                 IUnknown*) {
  trace(self, 9, "FindClosestMatchingMode", reinterpret_cast<uint64_t>(wanted), 0, 0);
  if (!closest) return E_INVALIDARG;
  fill_mode(*closest, self->width, self->height, wanted ? wanted->Format : DXGI_FORMAT_R8G8B8A8_UNORM);
  return S_OK;
}

HRESULT WINAPI output_wait_for_vblank(Output*) {
  // 60 Hz: wait for the next multiple of 1/60 s of the performance counter.
  LARGE_INTEGER f, now;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&now);
  const LONGLONG period = f.QuadPart / 60;
  const LONGLONG next = (now.QuadPart / period + 1) * period;
  const LONGLONG ms = (next - now.QuadPart) * 1000 / f.QuadPart;
  if (ms > 0) Sleep(static_cast<DWORD>(ms));
  return S_OK;
}

// Slot 21: the console's GetDisplayModeList1(format, flags, count, modes): DXGI_MODE_DESC1 has
// a Stereo flag after the fields of DXGI_MODE_DESC.
HRESULT WINAPI output_get_display_mode_list1(Output* self, DXGI_FORMAT format, UINT flags, UINT* count,
                                             DXGI_MODE_DESC1* modes) {
  trace(self, 21, "GetDisplayModeList1", static_cast<uint64_t>(format), flags, reinterpret_cast<uint64_t>(modes));
  if (!count) return E_INVALIDARG;
  if (!modes) {
    *count = 1;
    return S_OK;
  }
  if (*count < 1) return DXGI_ERROR_MORE_DATA;
  DXGI_MODE_DESC m;
  fill_mode(m, self->width, self->height, format);
  modes[0] = {};
  modes[0].Width = m.Width;
  modes[0].Height = m.Height;
  modes[0].RefreshRate = m.RefreshRate;
  modes[0].Format = m.Format;
  modes[0].ScanlineOrdering = m.ScanlineOrdering;
  modes[0].Scaling = m.Scaling;
  *count = 1;
  return S_OK;
}

const Kind& output_kind() {
  static const Kind k("dxgi output", 64, {
      {7, "GetDesc", reinterpret_cast<void*>(&output_get_desc)},
      {8, "GetDisplayModeList", reinterpret_cast<void*>(&output_get_display_mode_list)},
      {9, "FindClosestMatchingMode", reinterpret_cast<void*>(&output_find_closest_matching_mode)},
      {10, "WaitForVBlank", reinterpret_cast<void*>(&output_wait_for_vblank)},
      {21, "GetDisplayModeList1", reinterpret_cast<void*>(&output_get_display_mode_list1)},
  });
  return k;
}

// ---- adapter (IDXGIAdapter) ----

HRESULT WINAPI adapter_enum_outputs(Adapter* self, UINT index, Object** out) {
  trace(self, 7, "EnumOutputs", index, reinterpret_cast<uint64_t>(out), 0);
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  if (index != 0) return DXGI_ERROR_NOT_FOUND;
  add_ref(self->output);
  *out = self->output;
  return S_OK;
}

HRESULT WINAPI adapter_get_desc(Adapter* self, DXGI_ADAPTER_DESC* desc) {
  trace(self, 8, "GetDesc", reinterpret_cast<uint64_t>(desc), 0, 0);
  if (!desc) return E_INVALIDARG;
  *desc = {};
  wcscpy_s(desc->Description, L"Console GPU (Project Americas)");
  desc->VendorId = 0x1002;
  return S_OK;
}

const Kind& adapter_kind() {
  static const Kind k("dxgi adapter", 32, {
      {7, "EnumOutputs", reinterpret_cast<void*>(&adapter_enum_outputs)},
      {8, "GetDesc", reinterpret_cast<void*>(&adapter_get_desc)},
  });
  return k;
}

// ---- DXGI device (IDXGIDevice / IDXGIDevice1) ----

HRESULT WINAPI dxgi_get_parent(DxgiDevice* self, const GUID* riid, void** out) {
  trace(self, 6, "GetParent", 0, reinterpret_cast<uint64_t>(out), 0);
  if (!out) return E_INVALIDARG;
  (void)riid;
  add_ref(self->adapter);
  *out = self->adapter;
  return S_OK;
}

HRESULT WINAPI dxgi_get_adapter(DxgiDevice* self, Object** out) {
  trace(self, 7, "GetAdapter", reinterpret_cast<uint64_t>(out), 0, 0);
  if (!out) return E_INVALIDARG;
  add_ref(self->adapter);
  *out = self->adapter;
  return S_OK;
}

HRESULT WINAPI dxgi_set_gpu_thread_priority(DxgiDevice* self, INT priority) {
  trace(self, 10, "SetGPUThreadPriority", static_cast<uint64_t>(priority), 0, 0);
  if (priority < -7 || priority > 7) return E_INVALIDARG;
  self->gpu_priority = priority;
  return S_OK;
}

HRESULT WINAPI dxgi_get_gpu_thread_priority(DxgiDevice* self, INT* priority) {
  if (!priority) return E_POINTER;
  *priority = self->gpu_priority;
  return S_OK;
}

HRESULT WINAPI dxgi_set_maximum_frame_latency(DxgiDevice* self, UINT latency) {
  trace(self, 12, "SetMaximumFrameLatency", latency, 0, 0);
  if (latency > 16) return E_INVALIDARG;
  self->frame_latency = latency ? latency : 3;
  return S_OK;
}

HRESULT WINAPI dxgi_get_maximum_frame_latency(DxgiDevice* self, UINT* latency) {
  if (!latency) return E_POINTER;
  *latency = self->frame_latency;
  return S_OK;
}

const Kind& dxgi_device_kind() {
  static const Kind k("dxgi device", 32, {
      {6, "GetParent", reinterpret_cast<void*>(&dxgi_get_parent)},
      {7, "GetAdapter", reinterpret_cast<void*>(&dxgi_get_adapter)},
      {10, "SetGPUThreadPriority", reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority)},
      {11, "GetGPUThreadPriority", reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority)},
      {12, "SetMaximumFrameLatency", reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency)},
      {13, "GetMaximumFrameLatency", reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency)},
  });
  return k;
}

}  // namespace

bool is_dxgi_device_iid(const GUID& iid) {
  return IsEqualGUID(iid, kIDXGIObject) || IsEqualGUID(iid, kIDXGIDevice) || IsEqualGUID(iid, kIDXGIDevice1) ||
         IsEqualGUID(iid, kIDXGIDevice2) || IsEqualGUID(iid, kIDXGIDevice3);
}

Object* create_dxgi_device(Object* device) {
  auto* out = make_object<Output>(output_kind());
  DEVMODEW mode{};
  mode.dmSize = sizeof mode;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth && mode.dmPelsHeight) {
    out->width = mode.dmPelsWidth;
    out->height = mode.dmPelsHeight;
  }
  auto* adapter = make_object<Adapter>(adapter_kind());
  adapter->output = out;
  auto* d = make_object<DxgiDevice>(dxgi_device_kind());
  d->device = device;
  d->adapter = adapter;
  logf("d3d12x: DXGI view of the device; output %ux%u at 60 Hz", out->width, out->height);
  return d;
}

}  // namespace am::rt::d3d12x
