#include "runtime/present/window.h"

#include <algorithm>
#include <mutex>

#include "runtime/log.h"
#include "runtime/process.h"

namespace am::rt::present {
namespace {

constexpr wchar_t kClass[] = L"ProjectAmericasWindow";
constexpr wchar_t kName[] = L"Project Americas";

HWND g_window = nullptr;
constexpr UINT kSetStatus = WM_APP + 1;
std::mutex g_status_lock;
std::wstring g_status;

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  switch (message) {
    case WM_CLOSE:
      logf("window: closed; ending the run");
      if (TitleProcess* p = process()) p->write_report();
      TerminateProcess(GetCurrentProcess(), 0);
      return 0;
    case kSetStatus: {
      std::wstring text;
      {
        std::lock_guard g(g_status_lock);
        text = g_status;
      }
      SetWindowTextW(hwnd, text.c_str());
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // the swap chain covers the client area
    default:
      return DefWindowProcW(hwnd, message, w, l);
  }
}

// A 16:9 client area of about two thirds of the main display's work area, centred.
RECT initial_rect(DWORD style) {
  RECT work{0, 0, 1920, 1080};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const LONG work_w = work.right - work.left, work_h = work.bottom - work.top;
  LONG w = std::min<LONG>(work_w * 2 / 3, work_h * 3 / 4 * 16 / 9);
  w = std::max<LONG>(w, 640);
  const LONG h = w * 9 / 16;
  RECT r{0, 0, w, h};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  const LONG outer_w = r.right - r.left, outer_h = r.bottom - r.top;
  const LONG x = work.left + (work_w - outer_w) / 2, y = work.top + (work_h - outer_h) / 2;
  return RECT{x, y, x + outer_w, y + outer_h};
}

struct Start {
  HANDLE ready;
};

DWORD WINAPI window_thread(void* arg) {
  auto* start = static_cast<Start*>(arg);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetThreadDescription(GetCurrentThread(), L"Project Americas window");
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = &window_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  constexpr DWORD style = WS_OVERLAPPEDWINDOW;
  const RECT r = initial_rect(style);
  g_window = CreateWindowExW(0, kClass, kName, style, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr,
                             nullptr, wc.hInstance, nullptr);
  if (g_window) {
    ShowWindow(g_window, SW_SHOWNORMAL);
    UpdateWindow(g_window);
    logf("window: %ldx%ld", r.right - r.left, r.bottom - r.top);
  } else {
    logf("window: cannot make one (error %lu)", GetLastError());
  }
  SetEvent(start->ready);
  if (!g_window) return 1;
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}

}  // namespace

HWND game_window() {
  static std::once_flag once;
  std::call_once(once, [] {
    Start start{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (HANDLE t = CreateThread(nullptr, 0, &window_thread, &start, 0, nullptr)) {
      WaitForSingleObject(start.ready, INFINITE);
      CloseHandle(t);
    }
    CloseHandle(start.ready);
  });
  return g_window;
}

void set_window_status(const std::wstring& status) {
  if (!g_window) return;
  {
    std::lock_guard g(g_status_lock);
    g_status = status.empty() ? std::wstring(kName) : std::wstring(kName) + L" \u2014 " + status;
  }
  // Posted, never sent: the title's frame must not wait on the window's thread.
  PostMessageW(g_window, kSetStatus, 0, 0);
}

}  // namespace am::rt::present
