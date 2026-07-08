#pragma once
// System clipboard (plain text). navigator.clipboard is unreliable inside
// embedded web views (permission prompts never show), so the host does it.
//   Windows  Open/Get/SetClipboardData with CF_UNICODETEXT
//   macOS    NSPasteboard generalPasteboard via the objc runtime
//   Linux    GdkClipboard (async read made synchronous with a nested GMainLoop)
//
// Must run on the UI thread (Linux needs the GDK display; Windows/macOS are
// happier there too).
#include <optional>
#include <string>

#include "shell.hpp" // widen_utf8 / narrow_utf16 (Windows)

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#else
#include <gtk/gtk.h>
#endif

namespace clipboard {

#if defined(__APPLE__)
namespace detail {
using Send0   = id (*)(id, SEL);
using SendId  = id (*)(id, SEL, id);
using SendStr = id (*)(id, SEL, const char*);
inline id pasteboard() {
  return reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSPasteboard")), sel_registerName("generalPasteboard"));
}
inline id nsstr(const std::string& s) {
  return reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), s.c_str());
}
}
#endif

// nullopt = clipboard unavailable or holds no text.
inline std::optional<std::string> read_text(void* hwnd) {
#if defined(_WIN32)
  if (!OpenClipboard(static_cast<HWND>(hwnd))) return std::nullopt;
  std::optional<std::string> out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
      out = shell::narrow_utf16(w);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;

#elif defined(__APPLE__)
  (void)hwnd;
  using namespace detail;
  id str = reinterpret_cast<SendId>(objc_msgSend)(
      pasteboard(), sel_registerName("stringForType:"), nsstr("public.utf8-plain-text"));
  if (!str) return std::nullopt;
  using SendC = const char* (*)(id, SEL);
  const char* c = reinterpret_cast<SendC>(objc_msgSend)(str, sel_registerName("UTF8String"));
  return c ? std::optional<std::string>(c) : std::nullopt;

#else
  (void)hwnd;
  GdkDisplay* display = gdk_display_get_default();
  if (!display) return std::nullopt;
  GdkClipboard* cb = gdk_display_get_clipboard(display);
  struct Ctx { GMainLoop* loop; std::optional<std::string> text; } ctx{
      g_main_loop_new(nullptr, TRUE), std::nullopt};
  gdk_clipboard_read_text_async(cb, nullptr,
      +[](GObject* src, GAsyncResult* res, gpointer data) {
        auto* c = static_cast<Ctx*>(data);
        if (gchar* t = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr)) {
          c->text = t;
          g_free(t);
        }
        g_main_loop_quit(c->loop);
      }, &ctx);
  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);
  return ctx.text;
#endif
}

inline bool write_text(void* hwnd, const std::string& text) {
#if defined(_WIN32)
  if (!OpenClipboard(static_cast<HWND>(hwnd))) return false;
  bool ok = false;
  if (EmptyClipboard()) {
    const std::wstring w = shell::widen_utf8(text);
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
      if (void* p = GlobalLock(h)) {
        memcpy(p, w.c_str(), bytes);
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
      }
      if (!ok) GlobalFree(h); // ownership passes to the clipboard only on success
    }
  }
  CloseClipboard();
  return ok;

#elif defined(__APPLE__)
  (void)hwnd;
  using namespace detail;
  id pb = pasteboard();
  reinterpret_cast<long (*)(id, SEL)>(objc_msgSend)(pb, sel_registerName("clearContents"));
  using SetStr = bool (*)(id, SEL, id, id);
  return reinterpret_cast<SetStr>(objc_msgSend)(
      pb, sel_registerName("setString:forType:"),
      nsstr(text), nsstr("public.utf8-plain-text"));

#else
  (void)hwnd;
  GdkDisplay* display = gdk_display_get_default();
  if (!display) return false;
  gdk_clipboard_set_text(gdk_display_get_clipboard(display), text.c_str());
  return true;
#endif
}

} // namespace clipboard
