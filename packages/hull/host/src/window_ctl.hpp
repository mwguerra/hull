#pragma once
// Native fullscreen control for the host window. The webview library exposes no
// fullscreen API, so this drops to the native handle webview::window() returns:
// HWND (Windows), NSWindow* (macOS), GtkWindow* (Linux/GTK4). One host process
// owns exactly one window, so the Windows saved-placement state can be static.
//
// Must be called on the UI thread (use webview::dispatch from binding handlers).

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#else
#include <gtk/gtk.h>
#endif

namespace window_ctl {

#if defined(_WIN32)

namespace detail {
inline WINDOWPLACEMENT saved_placement{sizeof(WINDOWPLACEMENT)};
inline LONG_PTR saved_style = 0;
}

// Borderless fullscreen: WS_OVERLAPPEDWINDOW stripped = our fullscreen marker.
inline bool is_fullscreen(void* win) {
  HWND hwnd = static_cast<HWND>(win);
  if (!hwnd) return false;
  return (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == 0;
}

inline bool set_fullscreen(void* win, bool on) {
  HWND hwnd = static_cast<HWND>(win);
  if (!hwnd) return false;
  if (is_fullscreen(win) == on) return true;
  if (on) {
    detail::saved_style = GetWindowLongPtr(hwnd, GWL_STYLE);
    detail::saved_placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(hwnd, &detail::saved_placement);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    SetWindowLongPtr(hwnd, GWL_STYLE, detail::saved_style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    SetWindowLongPtr(hwnd, GWL_STYLE, detail::saved_style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(hwnd, &detail::saved_placement);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  return true;
}

#elif defined(__APPLE__)

// NSWindowStyleMaskFullScreen — set while the window is in (or entering) the
// macOS fullscreen Space.
inline bool is_fullscreen(void* win) {
  if (!win) return false;
  using SendULong = unsigned long (*)(id, SEL);
  const unsigned long mask = reinterpret_cast<SendULong>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("styleMask"));
  return (mask & (1UL << 14)) != 0;
}

// toggleFullScreen: animates asynchronously; is_fullscreen reflects the state
// once the transition is underway/complete.
inline bool set_fullscreen(void* win, bool on) {
  if (!win) return false;
  if (is_fullscreen(win) == on) return true;
  using SendId = void (*)(id, SEL, id);
  reinterpret_cast<SendId>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("toggleFullScreen:"), nullptr);
  return true;
}

#else // Linux / GTK4

inline bool is_fullscreen(void* win) {
  if (!win) return false;
  return gtk_window_is_fullscreen(GTK_WINDOW(win)) == TRUE;
}

inline bool set_fullscreen(void* win, bool on) {
  if (!win) return false;
  if (on) gtk_window_fullscreen(GTK_WINDOW(win));
  else gtk_window_unfullscreen(GTK_WINDOW(win));
  return true;
}

#endif

} // namespace window_ctl
