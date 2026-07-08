#pragma once
// Native window control for the host window: fullscreen, minimize/maximize,
// show/hide, center, always-on-top, bounds, focus. The webview library exposes
// none of this, so we drop to the native handle webview::window() returns:
// HWND (Windows), NSWindow* (macOS), GtkWindow* (Linux/GTK4). One host process
// owns exactly one window, so the Windows saved-placement state can be static.
//
// Platform reality on Linux/Wayland: the compositor owns window placement, so
// center(), always-on-top and window *positions* are unsupported there — those
// return false / omit fields, and the JS reply says so. Everything else works.
//
// Must be called on the UI thread (use webview::dispatch from binding handlers).

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CGGeometry.h>
#else
#include <gtk/gtk.h>
#endif

namespace window_ctl {

struct Bounds {
  bool has_position = false;
  int x = 0, y = 0, width = 0, height = 0;
};

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

inline bool minimize(void* win) {
  if (!win) return false;
  ShowWindow(static_cast<HWND>(win), SW_MINIMIZE);
  return true;
}

inline bool is_maximized(void* win) {
  return win && IsZoomed(static_cast<HWND>(win));
}

inline bool set_maximized(void* win, bool on) {
  if (!win) return false;
  ShowWindow(static_cast<HWND>(win), on ? SW_MAXIMIZE : SW_RESTORE);
  return true;
}

inline bool set_visible(void* win, bool on) {
  if (!win) return false;
  ShowWindow(static_cast<HWND>(win), on ? SW_SHOW : SW_HIDE);
  return true;
}

inline bool center(void* win) {
  HWND hwnd = static_cast<HWND>(win);
  if (!hwnd) return false;
  RECT r{};
  if (!GetWindowRect(hwnd, &r)) return false;
  MONITORINFO mi{}; mi.cbSize = sizeof(mi);
  if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
  const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
  return SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER) == TRUE;
}

inline bool set_always_on_top(void* win, bool on) {
  if (!win) return false;
  return SetWindowPos(static_cast<HWND>(win), on ? HWND_TOPMOST : HWND_NOTOPMOST,
                      0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE) == TRUE;
}

inline bool get_bounds(void* win, Bounds& out) {
  HWND hwnd = static_cast<HWND>(win);
  if (!hwnd) return false;
  RECT r{};
  if (!GetWindowRect(hwnd, &r)) return false;
  out.has_position = true;
  out.x = r.left; out.y = r.top;
  out.width = r.right - r.left; out.height = r.bottom - r.top;
  return true;
}

inline bool set_position(void* win, int x, int y) {
  if (!win) return false;
  return SetWindowPos(static_cast<HWND>(win), nullptr, x, y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER) == TRUE;
}

inline bool present(void* win) {
  HWND hwnd = static_cast<HWND>(win);
  if (!hwnd) return false;
  if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
  return true;
}

#elif defined(__APPLE__)

namespace detail {
using Send0 = id (*)(id, SEL);
using SendV = void (*)(id, SEL);
using SendVId = void (*)(id, SEL, id);
using SendB = bool (*)(id, SEL);
// CGRect (32 bytes) is returned in registers on arm64 but via stret on x86_64.
inline CGRect frame_of(id win) {
#if defined(__x86_64__)
  using SendRect = CGRect (*)(id, SEL);
  return reinterpret_cast<SendRect>(objc_msgSend_stret)(win, sel_registerName("frame"));
#else
  using SendRect = CGRect (*)(id, SEL);
  return reinterpret_cast<SendRect>(objc_msgSend)(win, sel_registerName("frame"));
#endif
}
inline void set_frame(id win, CGRect r) {
  using SendSetFrame = void (*)(id, SEL, CGRect, signed char);
  reinterpret_cast<SendSetFrame>(objc_msgSend)(win, sel_registerName("setFrame:display:"), r, 1);
}
inline double screen_height(id win) {
  id screen = reinterpret_cast<Send0>(objc_msgSend)(win, sel_registerName("screen"));
  if (!screen) {
    screen = reinterpret_cast<Send0>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSScreen")), sel_registerName("mainScreen"));
  }
  if (!screen) return 0;
#if defined(__x86_64__)
  using SendRect = CGRect (*)(id, SEL);
  const CGRect f = reinterpret_cast<SendRect>(objc_msgSend_stret)(screen, sel_registerName("frame"));
#else
  using SendRect = CGRect (*)(id, SEL);
  const CGRect f = reinterpret_cast<SendRect>(objc_msgSend)(screen, sel_registerName("frame"));
#endif
  return f.size.height;
}
}

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

inline bool minimize(void* win) {
  if (!win) return false;
  reinterpret_cast<detail::SendVId>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("miniaturize:"), nullptr);
  return true;
}

inline bool is_maximized(void* win) {
  if (!win) return false;
  return reinterpret_cast<detail::SendB>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("isZoomed"));
}

inline bool set_maximized(void* win, bool on) {
  if (!win) return false;
  if (is_maximized(win) == on) return true;
  reinterpret_cast<detail::SendVId>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("zoom:"), nullptr); // zoom: toggles
  return true;
}

inline bool set_visible(void* win, bool on) {
  if (!win) return false;
  if (on) {
    reinterpret_cast<detail::SendVId>(objc_msgSend)(
        reinterpret_cast<id>(win), sel_registerName("makeKeyAndOrderFront:"), nullptr);
  } else {
    reinterpret_cast<detail::SendVId>(objc_msgSend)(
        reinterpret_cast<id>(win), sel_registerName("orderOut:"), nullptr);
  }
  return true;
}

inline bool center(void* win) {
  if (!win) return false;
  reinterpret_cast<detail::SendV>(objc_msgSend)(reinterpret_cast<id>(win),
                                                sel_registerName("center"));
  return true;
}

inline bool set_always_on_top(void* win, bool on) {
  if (!win) return false;
  using SendLevel = void (*)(id, SEL, long);
  reinterpret_cast<SendLevel>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("setLevel:"), on ? 3L : 0L); // NSFloatingWindowLevel : NSNormalWindowLevel
  return true;
}

// Cocoa's origin is bottom-left; convert to the top-left coordinates JS expects.
inline bool get_bounds(void* win, Bounds& out) {
  if (!win) return false;
  id w = reinterpret_cast<id>(win);
  const CGRect f = detail::frame_of(w);
  const double sh = detail::screen_height(w);
  out.has_position = true;
  out.x = static_cast<int>(f.origin.x);
  out.y = static_cast<int>(sh - f.origin.y - f.size.height);
  out.width = static_cast<int>(f.size.width);
  out.height = static_cast<int>(f.size.height);
  return true;
}

inline bool set_position(void* win, int x, int y) {
  if (!win) return false;
  id w = reinterpret_cast<id>(win);
  CGRect f = detail::frame_of(w);
  const double sh = detail::screen_height(w);
  f.origin.x = x;
  f.origin.y = sh - y - f.size.height;
  detail::set_frame(w, f);
  return true;
}

inline bool present(void* win) {
  if (!win) return false;
  id app = reinterpret_cast<detail::Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSApplication")), sel_registerName("sharedApplication"));
  using SendBool = void (*)(id, SEL, signed char);
  if (app) {
    reinterpret_cast<SendBool>(objc_msgSend)(
        app, sel_registerName("activateIgnoringOtherApps:"), 1);
  }
  reinterpret_cast<detail::SendVId>(objc_msgSend)(
      reinterpret_cast<id>(win), sel_registerName("makeKeyAndOrderFront:"), nullptr);
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

inline bool minimize(void* win) {
  if (!win) return false;
  gtk_window_minimize(GTK_WINDOW(win));
  return true;
}

inline bool is_maximized(void* win) {
  if (!win) return false;
  return gtk_window_is_maximized(GTK_WINDOW(win)) == TRUE;
}

inline bool set_maximized(void* win, bool on) {
  if (!win) return false;
  if (on) gtk_window_maximize(GTK_WINDOW(win));
  else gtk_window_unmaximize(GTK_WINDOW(win));
  return true;
}

inline bool set_visible(void* win, bool on) {
  if (!win) return false;
  gtk_widget_set_visible(GTK_WIDGET(win), on ? TRUE : FALSE);
  return true;
}

// Wayland: the compositor owns placement — centering, always-on-top and window
// positions are not available to clients. Report unsupported honestly.
inline bool center(void*) { return false; }
inline bool set_always_on_top(void*, bool) { return false; }
inline bool set_position(void*, int, int) { return false; }

inline bool get_bounds(void* win, Bounds& out) {
  if (!win) return false;
  out.has_position = false; // no global coordinates on Wayland
  out.width = gtk_widget_get_width(GTK_WIDGET(win));
  out.height = gtk_widget_get_height(GTK_WIDGET(win));
  if (out.width == 0 && out.height == 0) {
    gtk_window_get_default_size(GTK_WINDOW(win), &out.width, &out.height);
  }
  return true;
}

inline bool present(void* win) {
  if (!win) return false;
  gtk_window_present(GTK_WINDOW(win));
  return true;
}

#endif

} // namespace window_ctl
