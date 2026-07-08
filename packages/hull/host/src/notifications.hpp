#pragma once
// System notifications — one implementation per OS, no new dependencies:
//
//   Windows  Shell_NotifyIcon balloon (NIF_INFO): shows as a toast and lands in
//            the Action Center on Win 10/11. Needs the app window's HWND, so it
//            requires window mode there (serve mode replies with an error; the
//            JS bridge uses the Web Notification API in browser dev anyway).
//   macOS    NSUserNotification via the objc runtime when the process has a
//            bundle id (packaged .app) — the notification is attributed to the
//            app. The raw dev binary has no bundle, where the notification
//            center API is unavailable; there we fall back to osascript's
//            `display notification`. (Deprecated API, same precedent as the
//            keychain — documented in platforms.md.)
//   Linux    org.freedesktop.Notifications over D-Bus via gio (already linked
//            for openExternal). The app icon path is passed through when set.
//
// Test hook: HULL_SHELL_DRYRUN=1 prints instead of notifying (same as shell.hpp).
#include <cstdlib>
#include <iostream>
#include <string>

#include "notify_text.hpp"
#include "emit_hook.hpp" // "notification:clicked" event
#include "shell.hpp" // dryrun(), widen_utf8 (Windows), spawn_detached (macOS fallback)

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include "tray.hpp" // shared tray icon + callback subclass (balloon clicks)
#elif defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#else
#include <gio/gio.h>
#endif

namespace notifications {

#if defined(__APPLE__)
namespace detail {
inline id nsstr(const std::string& s) {
  using SendStr = id (*)(id, SEL, const char*);
  return reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), s.c_str());
}

// NSUserNotificationCenter suppresses banners from the FRONTMOST app unless a
// delegate says otherwise — and "user clicks a button in the app" is exactly
// the frontmost case. The runtime-built delegate always presents, and turns
// notification activation into the "notification:clicked" event.
inline id presenting_delegate() {
  static id inst = nullptr;
  if (inst) return inst;
  Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "HullNotificationDelegate", 0);
  if (cls) {
    const auto present = reinterpret_cast<IMP>(
        +[](id, SEL, id, id) -> signed char { return 1; }); // BOOL YES
    class_addMethod(cls, sel_registerName("userNotificationCenter:shouldPresentNotification:"),
                    present, "c@:@@");
    const auto activated = reinterpret_cast<IMP>(+[](id, SEL, id, id) {
      hooks::emit("notification:clicked", nlohmann::json::object());
    });
    class_addMethod(cls, sel_registerName("userNotificationCenter:didActivateNotification:"),
                    activated, "v@:@@");
    objc_registerClassPair(cls);
    inst = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
        reinterpret_cast<id>(cls), sel_registerName("new"));
  }
  return inst;
}
}
#endif

// Show a notification. `hwnd` is the native window handle (used on Windows
// only; pass nullptr elsewhere/when absent). `app_name`/`icon_path` brand the
// notification where the platform supports it.
inline bool show(void* hwnd, const std::string& title, const std::string& body,
                 const std::string& app_name, const std::string& icon_path) {
  if (shell::dryrun()) {
    std::cerr << "hull-host: [dry-run] notify " << title << " :: " << body << "\n";
    return true;
  }

#if defined(_WIN32)
  (void)icon_path;
  HWND owner = static_cast<HWND>(hwnd);
  if (!owner) return false; // balloon icons need a window to attach to
  // The tray module owns the icon (shared with traySet) and routes its
  // callbacks — including NIN_BALLOONUSERCLICK -> "notification:clicked".
  if (!tray::ensure_icon(owner, app_name)) return false;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = 0x48554C; // "HUL" — same icon as tray::ensure_icon
  auto copy = [](wchar_t* dst, size_t cap, const std::wstring& s) {
    const size_t len = s.size() < cap - 1 ? s.size() : cap - 1;
    wmemcpy(dst, s.c_str(), len);
    dst[len] = L'\0';
  };
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO;
  // An empty szInfo hides the balloon — promote the title to the body slot.
  const bool no_body = body.empty();
  copy(nid.szInfo, 256, shell::widen_utf8(no_body ? title : body));
  copy(nid.szInfoTitle, 64, shell::widen_utf8(no_body ? app_name : title));
  if (Shell_NotifyIconW(NIM_MODIFY, &nid)) return true;
  // An Explorer restart drops tray icons and NIM_MODIFY fails forever after —
  // re-add the icon and retry once.
  tray::reset_icon_state();
  if (!tray::ensure_icon(owner, app_name)) return false;
  return Shell_NotifyIconW(NIM_MODIFY, &nid) == TRUE;

#elif defined(__APPLE__)
  (void)hwnd; (void)app_name; (void)icon_path;
  using Send0 = id (*)(id, SEL);
  using SetId = void (*)(id, SEL, id);
  // The notification center needs a bundle id (packaged .app). The raw dev
  // binary has none — use the osascript fallback there.
  id bundle = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSBundle")), sel_registerName("mainBundle"));
  id bid = bundle ? reinterpret_cast<Send0>(objc_msgSend)(
      bundle, sel_registerName("bundleIdentifier")) : nullptr;
  if (bid) {
    id center = reinterpret_cast<Send0>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSUserNotificationCenter")),
        sel_registerName("defaultUserNotificationCenter"));
    if (center) {
      id delegate = detail::presenting_delegate();
      if (delegate) {
        reinterpret_cast<SetId>(objc_msgSend)(center, sel_registerName("setDelegate:"), delegate);
      }
      id note = reinterpret_cast<Send0>(objc_msgSend)(
          reinterpret_cast<Send0>(objc_msgSend)(
              reinterpret_cast<id>(objc_getClass("NSUserNotification")),
              sel_registerName("alloc")),
          sel_registerName("init"));
      if (!note) return false;
      reinterpret_cast<SetId>(objc_msgSend)(note, sel_registerName("setTitle:"),
                                            detail::nsstr(title));
      if (!body.empty()) {
        reinterpret_cast<SetId>(objc_msgSend)(note, sel_registerName("setInformativeText:"),
                                              detail::nsstr(body));
      }
      reinterpret_cast<SetId>(objc_msgSend)(center, sel_registerName("deliverNotification:"), note);
      // deliverNotification: copies the notification — release our alloc.
      reinterpret_cast<void (*)(id, SEL)>(objc_msgSend)(note, sel_registerName("release"));
      return true;
    }
  }
  const std::string script = "display notification \"" + notify_text::applescript_escape(body) +
                             "\" with title \"" + notify_text::applescript_escape(title) + "\"";
  return shell::spawn_detached({"/usr/bin/osascript", "-e", script});

#else // Linux
  (void)hwnd;
  GError* err = nullptr;
  // One process-lifetime connection: it also carries the ActionInvoked signal
  // subscription that turns a clicked notification into "notification:clicked".
  static GDBusConnection* conn = nullptr;
  static guint32 last_id = 0;
  if (!conn) {
    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!conn) { if (err) g_error_free(err); return false; }
    g_dbus_connection_signal_subscribe(
        conn, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
        "ActionInvoked", "/org/freedesktop/Notifications", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
            GVariant* params, gpointer user_data) {
          guint32 id = 0;
          const gchar* action = nullptr;
          g_variant_get(params, "(u&s)", &id, &action);
          const guint32 ours = *static_cast<guint32*>(user_data);
          if (id == ours && action && std::string(action) == "default") {
            hooks::emit("notification:clicked", nlohmann::json::object());
          }
        },
        &last_id, nullptr);
  }
  // The "default" action is what makes the notification clickable at all.
  GVariantBuilder actions;
  g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
  g_variant_builder_add(&actions, "s", "default");
  g_variant_builder_add(&actions, "s", "Open");
  GVariant* ret = g_dbus_connection_call_sync(
      conn, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "Notify",
      g_variant_new("(susssasa{sv}i)",
                    app_name.c_str(), (guint32)0, icon_path.c_str(),
                    title.c_str(), body.c_str(),
                    &actions, nullptr, (gint32)-1),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);
  if (!ret) { if (err) g_error_free(err); return false; }
  g_variant_get(ret, "(u)", &last_id);
  g_variant_unref(ret);
  return true;
#endif
}

} // namespace notifications
