#pragma once
// System tray / status item with a menu.
//   Windows  Shell_NotifyIcon icon (shared with notifications.hpp) + a window
//            subclass that receives icon callbacks; right-click shows a popup
//            menu built from the JSON model. Balloon clicks also arrive here
//            and become "notification:clicked".
//   macOS    NSStatusBar status item + NSMenu via the objc runtime; menu item
//            actions land on a runtime target class.
//   Linux    unsupported (a modern tray means implementing the D-Bus
//            StatusNotifierItem + dbusmenu specs, or depending on
//            libappindicator — neither fits Hull's zero-extra-deps host).
//            traySet replies { ok: false } with that explanation.
//
// Menu model (JSON): [{ id, label, type: "normal"|"separator"|"checkbox",
//                       checked?, enabled? }]
// Events: "tray:click" (icon left-click; Windows only — macOS opens the menu),
//         "tray:menu" { id } (a menu item was chosen).
//
// Must run on the UI thread.
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "emit_hook.hpp"
#include "shell.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#elif defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CGGeometry.h>
#endif

namespace tray {

struct MenuItem {
  std::string id;
  std::string label;
  std::string type = "normal"; // normal | separator | checkbox
  bool checked = false;
  bool enabled = true;
};

inline std::vector<MenuItem> parse_menu(const nlohmann::json& menu) {
  std::vector<MenuItem> items;
  if (!menu.is_array()) return items;
  for (const auto& m : menu) {
    if (!m.is_object()) continue;
    MenuItem it;
    it.id = m.value("id", "");
    it.label = m.value("label", "");
    it.type = m.value("type", "normal");
    it.checked = m.value("checked", false);
    it.enabled = m.value("enabled", true);
    items.push_back(it);
  }
  return items;
}

#if defined(_WIN32)

namespace detail {
constexpr UINT ICON_UID = 0x48554C;        // "HUL" — shared with notifications.hpp
constexpr UINT CALLBACK_MSG = WM_APP + 0x48;
constexpr UINT_PTR SUBCLASS_ID = 0x48554C;
constexpr UINT MENU_ID_BASE = 1000;

inline std::mutex& menu_mutex() { static std::mutex m; return m; }
inline std::vector<MenuItem>& menu_model() { static std::vector<MenuItem> m; return m; }
inline bool& icon_added() { static bool b = false; return b; }
inline bool& subclassed() { static bool b = false; return b; }

inline void show_menu(HWND hwnd) {
  std::vector<MenuItem> model;
  { std::lock_guard<std::mutex> lk(menu_mutex()); model = menu_model(); }
  if (model.empty()) return;
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < model.size(); ++i) {
    const auto& it = model[i];
    if (it.type == "separator") { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); continue; }
    UINT flags = MF_STRING;
    if (!it.enabled) flags |= MF_GRAYED;
    if (it.type == "checkbox" && it.checked) flags |= MF_CHECKED;
    AppendMenuW(menu, flags, MENU_ID_BASE + i, shell::widen_utf8(it.label).c_str());
  }
  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd); // required or the menu won't dismiss on outside clicks
  const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
  if (cmd >= MENU_ID_BASE && cmd < MENU_ID_BASE + model.size()) {
    hooks::emit("tray:menu", {{"id", model[cmd - MENU_ID_BASE].id}});
  }
}

inline LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR) {
  if (msg == CALLBACK_MSG) {
    switch (LOWORD(lp)) {
      // Under NOTIFYICON_VERSION_4 a left-click is NIN_SELECT (not WM_LBUTTONUP);
      // keep WM_LBUTTONUP too so the handler is version-agnostic.
      case NIN_SELECT:
      case WM_LBUTTONUP: hooks::emit("tray:click", nlohmann::json::object()); break;
      case WM_RBUTTONUP:
      case WM_CONTEXTMENU: show_menu(hwnd); break;
      case NIN_BALLOONUSERCLICK: hooks::emit("notification:clicked", nlohmann::json::object()); break;
      default: break;
    }
    return 0;
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}
}

// Add (once) the shared tray icon with callbacks routed to our subclass.
// Also used by notifications.hpp so balloons and the tray share one icon.
inline bool ensure_icon(void* hwnd_ptr, const std::string& tooltip) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd) return false;
  if (!detail::subclassed()) {
    if (!SetWindowSubclass(hwnd, detail::tray_proc, detail::SUBCLASS_ID, 0)) return false;
    detail::subclassed() = true;
  }
  if (detail::icon_added()) return true;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = detail::ICON_UID;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  nid.uCallbackMessage = detail::CALLBACK_MSG;
  HICON icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
  nid.hIcon = icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
  const std::wstring tip = shell::widen_utf8(tooltip);
  const size_t len = tip.size() < 127 ? tip.size() : 127;
  wmemcpy(nid.szTip, tip.c_str(), len);
  nid.szTip[len] = L'\0';
  if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
  nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid);
  detail::icon_added() = true;
  return true;
}

inline void reset_icon_state() { detail::icon_added() = false; } // after Explorer restart

inline bool set(void* hwnd, const std::string& tooltip, const std::string& /*icon_path*/,
                const std::vector<MenuItem>& items) {
  if (!ensure_icon(hwnd, tooltip)) return false;
  std::lock_guard<std::mutex> lk(detail::menu_mutex());
  detail::menu_model() = items;
  return true;
}

inline bool remove(void* hwnd) {
  if (!detail::icon_added()) return true;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = static_cast<HWND>(hwnd);
  nid.uID = detail::ICON_UID;
  const bool ok = Shell_NotifyIconW(NIM_DELETE, &nid) == TRUE;
  detail::icon_added() = false;
  return ok;
}

#elif defined(__APPLE__)

namespace detail {
using Send0   = id (*)(id, SEL);
using SendId  = id (*)(id, SEL, id);
using SendStr = id (*)(id, SEL, const char*);
using SendV   = void (*)(id, SEL, id);

inline id nsstr(const std::string& s) {
  return reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), s.c_str());
}
inline id& status_item() { static id item = nullptr; return item; }

// Runtime target class: every menu item's action lands in hullMenuAction: and
// the item's representedObject (the JSON id) becomes the tray:menu payload.
inline id menu_target() {
  static id inst = nullptr;
  if (inst) return inst;
  Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "HullTrayTarget", 0);
  if (cls) {
    const auto imp = reinterpret_cast<IMP>(+[](id, SEL, id sender) {
      id rep = reinterpret_cast<Send0>(objc_msgSend)(sender, sel_registerName("representedObject"));
      if (rep) {
        using SendC = const char* (*)(id, SEL);
        const char* c = reinterpret_cast<SendC>(objc_msgSend)(rep, sel_registerName("UTF8String"));
        hooks::emit("tray:menu", {{"id", c ? c : ""}});
      }
    });
    class_addMethod(cls, sel_registerName("hullMenuAction:"), imp, "v@:@");
    objc_registerClassPair(cls);
    inst = reinterpret_cast<Send0>(objc_msgSend)(reinterpret_cast<id>(cls), sel_registerName("new"));
  }
  return inst;
}
}

inline bool set(void* /*hwnd*/, const std::string& tooltip, const std::string& icon_path,
                const std::vector<MenuItem>& items) {
  using namespace detail;
  id bar = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSStatusBar")), sel_registerName("systemStatusBar"));
  if (!status_item()) {
    using SendF = id (*)(id, SEL, double);
    id item = reinterpret_cast<SendF>(objc_msgSend)(
        bar, sel_registerName("statusItemWithLength:"), -1.0); // NSVariableStatusItemLength
    if (!item) return false;
    reinterpret_cast<Send0>(objc_msgSend)(item, sel_registerName("retain"));
    status_item() = item;
  }
  id button = reinterpret_cast<Send0>(objc_msgSend)(status_item(), sel_registerName("button"));
  bool icon_set = false;
  if (!icon_path.empty()) {
    id img = reinterpret_cast<SendId>(objc_msgSend)(
        reinterpret_cast<Send0>(objc_msgSend)(
            reinterpret_cast<id>(objc_getClass("NSImage")), sel_registerName("alloc")),
        sel_registerName("initWithContentsOfFile:"), nsstr(icon_path));
    if (img) {
      using SetSize = void (*)(id, SEL, CGSize);
      reinterpret_cast<SetSize>(objc_msgSend)(img, sel_registerName("setSize:"), CGSize{18, 18});
      if (button) reinterpret_cast<SendV>(objc_msgSend)(button, sel_registerName("setImage:"), img);
      icon_set = true;
    }
  }
  if (!icon_set && button) {
    const std::string glyph = tooltip.empty() ? "\xE2\x97\x8F" /* ● */ : tooltip.substr(0, 12);
    reinterpret_cast<SendV>(objc_msgSend)(button, sel_registerName("setTitle:"), nsstr(glyph));
  }
  if (button && !tooltip.empty()) {
    reinterpret_cast<SendV>(objc_msgSend)(button, sel_registerName("setToolTip:"), nsstr(tooltip));
  }

  id menu = reinterpret_cast<SendId>(objc_msgSend)(
      reinterpret_cast<Send0>(objc_msgSend)(
          reinterpret_cast<id>(objc_getClass("NSMenu")), sel_registerName("alloc")),
      sel_registerName("initWithTitle:"), nsstr(""));
  using SetBool = void (*)(id, SEL, signed char);
  reinterpret_cast<SetBool>(objc_msgSend)(menu, sel_registerName("setAutoenablesItems:"), 0);
  for (const auto& it : items) {
    if (it.type == "separator") {
      id sep = reinterpret_cast<Send0>(objc_msgSend)(
          reinterpret_cast<id>(objc_getClass("NSMenuItem")), sel_registerName("separatorItem"));
      reinterpret_cast<SendV>(objc_msgSend)(menu, sel_registerName("addItem:"), sep);
      continue;
    }
    using InitItem = id (*)(id, SEL, id, SEL, id);
    id mi = reinterpret_cast<InitItem>(objc_msgSend)(
        reinterpret_cast<Send0>(objc_msgSend)(
            reinterpret_cast<id>(objc_getClass("NSMenuItem")), sel_registerName("alloc")),
        sel_registerName("initWithTitle:action:keyEquivalent:"),
        nsstr(it.label), sel_registerName("hullMenuAction:"), nsstr(""));
    reinterpret_cast<SendV>(objc_msgSend)(mi, sel_registerName("setTarget:"), menu_target());
    reinterpret_cast<SendV>(objc_msgSend)(mi, sel_registerName("setRepresentedObject:"), nsstr(it.id));
    reinterpret_cast<SetBool>(objc_msgSend)(mi, sel_registerName("setEnabled:"), it.enabled ? 1 : 0);
    if (it.type == "checkbox") {
      using SetState = void (*)(id, SEL, long);
      reinterpret_cast<SetState>(objc_msgSend)(mi, sel_registerName("setState:"), it.checked ? 1L : 0L);
    }
    reinterpret_cast<SendV>(objc_msgSend)(menu, sel_registerName("addItem:"), mi);
  }
  reinterpret_cast<SendV>(objc_msgSend)(status_item(), sel_registerName("setMenu:"), menu);
  return true;
}

inline bool remove(void* /*hwnd*/) {
  using namespace detail;
  if (!status_item()) return true;
  id bar = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSStatusBar")), sel_registerName("systemStatusBar"));
  reinterpret_cast<SendV>(objc_msgSend)(bar, sel_registerName("removeStatusItem:"), status_item());
  status_item() = nullptr;
  return true;
}

#else // Linux

inline bool set(void*, const std::string&, const std::string&, const std::vector<MenuItem>&) {
  return false; // StatusNotifierItem/dbusmenu not implemented — see header comment
}
inline bool remove(void*) { return false; }

#endif

} // namespace tray
