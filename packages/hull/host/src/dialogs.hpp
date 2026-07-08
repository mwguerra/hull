#pragma once
// Native dialogs: file open/save (+ folder picker) and message boxes.
//   Windows  IFileOpenDialog / IFileSaveDialog (COM) + MessageBoxW
//   macOS    NSOpenPanel / NSSavePanel / NSAlert via the objc runtime
//   Linux    GtkFileDialog / GtkAlertDialog (GTK 4.10+, async) made synchronous
//            with a nested GMainLoop
//
// All of these are MODAL and must run on the UI thread (run_on_ui) — they block
// it while open, which is exactly what a modal dialog does.
//
// Test hook: HULL_SHELL_DRYRUN=1 returns canned results (no dialog shows) so
// the e2e suite can drive these headlessly.
#include <optional>
#include <string>
#include <vector>

#include "shell.hpp" // dryrun(), widen_utf8/narrow_utf16 (Windows)

#if defined(_WIN32)
#include <windows.h>
#include <shobjidl.h>
#elif defined(__APPLE__)
#include <objc/runtime.h>
#include <objc/message.h>
#else
#include <gtk/gtk.h>
#endif

namespace dialogs {

struct Filter {
  std::string name;                     // "Images"
  std::vector<std::string> extensions;  // ["png", "jpg"] (no dots)
};

struct OpenOptions {
  std::string title;
  bool directory = false;  // pick folder(s) instead of file(s)
  bool multiple = false;
  std::vector<Filter> filters;
};

struct MessageOptions {
  std::string title;
  std::string message;
  std::string detail;
  std::vector<std::string> buttons;  // first = index 0; empty -> ["OK"]
  std::string type;                  // "info" | "warning" | "error"
  int default_id = 0;
};

// ---------------------------------------------------------------- helpers ---
#if defined(__APPLE__)
namespace detail {
using Send0   = id (*)(id, SEL);
using SendId  = id (*)(id, SEL, id);
using SendStr = id (*)(id, SEL, const char*);
using SendV   = void (*)(id, SEL, id);
using SendB   = void (*)(id, SEL, signed char);
using SendL   = long (*)(id, SEL);

inline id nsstr(const std::string& s) {
  return reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), s.c_str());
}
inline std::string to_utf8(id nsString) {
  if (!nsString) return {};
  using SendC = const char* (*)(id, SEL);
  const char* c = reinterpret_cast<SendC>(objc_msgSend)(nsString, sel_registerName("UTF8String"));
  return c ? c : "";
}
inline std::string path_of_url(id url) {
  return to_utf8(reinterpret_cast<Send0>(objc_msgSend)(url, sel_registerName("path")));
}
// NSArray<NSString*> of the filters' extensions (setAllowedFileTypes: —
// deprecated but dependable; same precedent as the keychain APIs).
inline id extensions_array(const std::vector<Filter>& filters) {
  id arr = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSMutableArray")), sel_registerName("array"));
  bool any = false;
  for (const auto& f : filters) {
    for (const auto& ext : f.extensions) {
      reinterpret_cast<SendV>(objc_msgSend)(arr, sel_registerName("addObject:"), nsstr(ext));
      any = true;
    }
  }
  return any ? arr : nullptr;
}
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
namespace detail {
// GTK4 dialogs are async-only; run a nested main loop until the callback fires.
struct SyncCtx {
  GMainLoop* loop = nullptr;
  std::vector<std::string> paths;
  int button = -1;
  bool ok = false;
};
inline GListStore* filter_store(const std::vector<Filter>& filters) {
  if (filters.empty()) return nullptr;
  GListStore* store = g_list_store_new(GTK_TYPE_FILE_FILTER);
  for (const auto& f : filters) {
    GtkFileFilter* ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, f.name.empty() ? "Files" : f.name.c_str());
    for (const auto& ext : f.extensions) gtk_file_filter_add_suffix(ff, ext.c_str());
    g_list_store_append(store, ff);
    g_object_unref(ff);
  }
  return store;
}
inline void collect_file(SyncCtx* c, GFile* f) {
  if (!f) return;
  gchar* p = g_file_get_path(f);
  if (p) { c->paths.emplace_back(p); g_free(p); }
  g_object_unref(f);
}
}
#endif

// ---------------------------------------------------------------- open ------
// nullopt = user canceled; otherwise the selected path(s).
inline std::optional<std::vector<std::string>> open(void* parent, const OpenOptions& o) {
  if (shell::dryrun()) {
    std::cerr << "hull-host: [dry-run] dialog-open" << (o.directory ? " (folder)" : "")
              << (o.multiple ? " (multiple)" : "") << "\n";
    return std::vector<std::string>{o.directory ? "/dry-run/folder" : "/dry-run/picked.txt"};
  }

#if defined(_WIN32)
  (void)parent;
  const bool needs_uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  IFileOpenDialog* dlg = nullptr;
  std::optional<std::vector<std::string>> result;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg)))) {
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    if (o.directory) flags |= FOS_PICKFOLDERS;
    if (o.multiple) flags |= FOS_ALLOWMULTISELECT;
    dlg->SetOptions(flags | FOS_FORCEFILESYSTEM);
    if (!o.title.empty()) dlg->SetTitle(shell::widen_utf8(o.title).c_str());
    // Filters (files only): "Name" / "*.png;*.jpg"
    std::vector<std::wstring> names, specs;
    std::vector<COMDLG_FILTERSPEC> fspecs;
    if (!o.directory && !o.filters.empty()) {
      for (const auto& f : o.filters) {
        std::string spec;
        for (const auto& ext : f.extensions) spec += (spec.empty() ? "*." : ";*.") + ext;
        names.push_back(shell::widen_utf8(f.name.empty() ? "Files" : f.name));
        specs.push_back(shell::widen_utf8(spec.empty() ? "*.*" : spec));
      }
      for (size_t i = 0; i < names.size(); ++i) fspecs.push_back({names[i].c_str(), specs[i].c_str()});
      dlg->SetFileTypes(static_cast<UINT>(fspecs.size()), fspecs.data());
    }
    if (SUCCEEDED(dlg->Show(static_cast<HWND>(parent)))) {
      IShellItemArray* items = nullptr;
      if (SUCCEEDED(dlg->GetResults(&items)) && items) {
        DWORD count = 0;
        items->GetCount(&count);
        std::vector<std::string> paths;
        for (DWORD i = 0; i < count; ++i) {
          IShellItem* item = nullptr;
          if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
              paths.push_back(shell::narrow_utf16(p));
              CoTaskMemFree(p);
            }
            item->Release();
          }
        }
        items->Release();
        result = paths;
      }
    }
    dlg->Release();
  }
  if (needs_uninit) CoUninitialize();
  return result;

#elif defined(__APPLE__)
  (void)parent; // panels attach to the key window on their own
  using namespace detail;
  id panel = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSOpenPanel")), sel_registerName("openPanel"));
  reinterpret_cast<SendB>(objc_msgSend)(panel, sel_registerName("setCanChooseFiles:"), !o.directory);
  reinterpret_cast<SendB>(objc_msgSend)(panel, sel_registerName("setCanChooseDirectories:"), o.directory);
  reinterpret_cast<SendB>(objc_msgSend)(panel, sel_registerName("setAllowsMultipleSelection:"), o.multiple);
  if (!o.title.empty()) {
    reinterpret_cast<SendV>(objc_msgSend)(panel, sel_registerName("setMessage:"), nsstr(o.title));
  }
  if (!o.directory) {
    if (id exts = extensions_array(o.filters)) {
      reinterpret_cast<SendV>(objc_msgSend)(panel, sel_registerName("setAllowedFileTypes:"), exts);
    }
  }
  const long response = reinterpret_cast<SendL>(objc_msgSend)(panel, sel_registerName("runModal"));
  if (response != 1 /* NSModalResponseOK */) return std::nullopt;
  id urls = reinterpret_cast<Send0>(objc_msgSend)(panel, sel_registerName("URLs"));
  using SendCount = unsigned long (*)(id, SEL);
  using SendAt = id (*)(id, SEL, unsigned long);
  const unsigned long count = reinterpret_cast<SendCount>(objc_msgSend)(urls, sel_registerName("count"));
  std::vector<std::string> paths;
  for (unsigned long i = 0; i < count; ++i) {
    id url = reinterpret_cast<SendAt>(objc_msgSend)(urls, sel_registerName("objectAtIndex:"), i);
    const std::string p = path_of_url(url);
    if (!p.empty()) paths.push_back(p);
  }
  return paths;

#else
  using namespace detail;
  SyncCtx ctx;
  ctx.loop = g_main_loop_new(nullptr, TRUE);
  GtkFileDialog* dlg = gtk_file_dialog_new();
  if (!o.title.empty()) gtk_file_dialog_set_title(dlg, o.title.c_str());
  GListStore* filters = o.directory ? nullptr : filter_store(o.filters);
  if (filters) gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
  GtkWindow* win = parent ? GTK_WINDOW(parent) : nullptr;

  if (o.directory) {
    gtk_file_dialog_select_folder(dlg, win, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
          auto* c = static_cast<SyncCtx*>(data);
          collect_file(c, gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr));
          g_main_loop_quit(c->loop);
        }, &ctx);
  } else if (o.multiple) {
    gtk_file_dialog_open_multiple(dlg, win, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
          auto* c = static_cast<SyncCtx*>(data);
          GListModel* files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, nullptr);
          if (files) {
            const guint n = g_list_model_get_n_items(files);
            for (guint i = 0; i < n; ++i) {
              collect_file(c, G_FILE(g_list_model_get_item(files, i)));
            }
            g_object_unref(files);
          }
          g_main_loop_quit(c->loop);
        }, &ctx);
  } else {
    gtk_file_dialog_open(dlg, win, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
          auto* c = static_cast<SyncCtx*>(data);
          collect_file(c, gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr));
          g_main_loop_quit(c->loop);
        }, &ctx);
  }
  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);
  g_object_unref(dlg);
  if (filters) g_object_unref(filters);
  if (ctx.paths.empty()) return std::nullopt; // canceled
  return ctx.paths;
#endif
}

// ---------------------------------------------------------------- save ------
inline std::optional<std::string> save(void* parent, const std::string& title,
                                       const std::string& default_name,
                                       const std::vector<Filter>& filters) {
  if (shell::dryrun()) {
    std::cerr << "hull-host: [dry-run] dialog-save " << default_name << "\n";
    return "/dry-run/" + (default_name.empty() ? "untitled" : default_name);
  }

#if defined(_WIN32)
  (void)parent;
  const bool needs_uninit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  IFileSaveDialog* dlg = nullptr;
  std::optional<std::string> result;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg)))) {
    if (!title.empty()) dlg->SetTitle(shell::widen_utf8(title).c_str());
    if (!default_name.empty()) dlg->SetFileName(shell::widen_utf8(default_name).c_str());
    std::vector<std::wstring> names, specs;
    std::vector<COMDLG_FILTERSPEC> fspecs;
    if (!filters.empty()) {
      for (const auto& f : filters) {
        std::string spec;
        for (const auto& ext : f.extensions) spec += (spec.empty() ? "*." : ";*.") + ext;
        names.push_back(shell::widen_utf8(f.name.empty() ? "Files" : f.name));
        specs.push_back(shell::widen_utf8(spec.empty() ? "*.*" : spec));
      }
      for (size_t i = 0; i < names.size(); ++i) fspecs.push_back({names[i].c_str(), specs[i].c_str()});
      dlg->SetFileTypes(static_cast<UINT>(fspecs.size()), fspecs.data());
    }
    if (SUCCEEDED(dlg->Show(static_cast<HWND>(parent)))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item)) && item) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
          result = shell::narrow_utf16(p);
          CoTaskMemFree(p);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (needs_uninit) CoUninitialize();
  return result;

#elif defined(__APPLE__)
  (void)parent;
  using namespace detail;
  id panel = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSSavePanel")), sel_registerName("savePanel"));
  if (!title.empty()) {
    reinterpret_cast<SendV>(objc_msgSend)(panel, sel_registerName("setMessage:"), nsstr(title));
  }
  if (!default_name.empty()) {
    reinterpret_cast<SendV>(objc_msgSend)(
        panel, sel_registerName("setNameFieldStringValue:"), nsstr(default_name));
  }
  if (id exts = extensions_array(filters)) {
    reinterpret_cast<SendV>(objc_msgSend)(panel, sel_registerName("setAllowedFileTypes:"), exts);
  }
  const long response = reinterpret_cast<SendL>(objc_msgSend)(panel, sel_registerName("runModal"));
  if (response != 1 /* NSModalResponseOK */) return std::nullopt;
  id url = reinterpret_cast<Send0>(objc_msgSend)(panel, sel_registerName("URL"));
  const std::string p = path_of_url(url);
  return p.empty() ? std::nullopt : std::optional<std::string>(p);

#else
  using namespace detail;
  SyncCtx ctx;
  ctx.loop = g_main_loop_new(nullptr, TRUE);
  GtkFileDialog* dlg = gtk_file_dialog_new();
  if (!title.empty()) gtk_file_dialog_set_title(dlg, title.c_str());
  if (!default_name.empty()) gtk_file_dialog_set_initial_name(dlg, default_name.c_str());
  GListStore* fstore = filter_store(filters);
  if (fstore) gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(fstore));
  gtk_file_dialog_save(dlg, parent ? GTK_WINDOW(parent) : nullptr, nullptr,
      +[](GObject* src, GAsyncResult* res, gpointer data) {
        auto* c = static_cast<SyncCtx*>(data);
        collect_file(c, gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr));
        g_main_loop_quit(c->loop);
      }, &ctx);
  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);
  g_object_unref(dlg);
  if (fstore) g_object_unref(fstore);
  if (ctx.paths.empty()) return std::nullopt;
  return ctx.paths.front();
#endif
}

// ---------------------------------------------------------------- message ---
// Returns the index of the pressed button (into o.buttons), or -1 on failure.
// Windows caveat: MessageBoxW supports fixed button sets, so the button COUNT
// maps to OK / OK-Cancel / Yes-No-Cancel and the labels are the system's.
inline int message(void* parent, const MessageOptions& o) {
  const std::vector<std::string> buttons = o.buttons.empty()
      ? std::vector<std::string>{"OK"} : o.buttons;
  if (shell::dryrun()) {
    std::cerr << "hull-host: [dry-run] dialog-message " << o.message << "\n";
    return o.default_id >= 0 && o.default_id < static_cast<int>(buttons.size()) ? o.default_id : 0;
  }

#if defined(_WIN32)
  UINT type = MB_OK;
  if (buttons.size() == 2) type = MB_OKCANCEL;
  else if (buttons.size() >= 3) type = MB_YESNOCANCEL;
  if (o.type == "warning") type |= MB_ICONWARNING;
  else if (o.type == "error") type |= MB_ICONERROR;
  else type |= MB_ICONINFORMATION;
  if (o.default_id == 1) type |= MB_DEFBUTTON2;
  else if (o.default_id == 2) type |= MB_DEFBUTTON3;
  std::string text = o.message;
  if (!o.detail.empty()) text += "\n\n" + o.detail;
  const int r = MessageBoxW(static_cast<HWND>(parent), shell::widen_utf8(text).c_str(),
                            shell::widen_utf8(o.title.empty() ? "Message" : o.title).c_str(), type);
  switch (r) {
    case IDOK: case IDYES: return 0;
    case IDNO: return 1;
    case IDCANCEL: return static_cast<int>(buttons.size()) - 1;
    default: return -1;
  }

#elif defined(__APPLE__)
  (void)parent;
  using namespace detail;
  id alert = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<Send0>(objc_msgSend)(
          reinterpret_cast<id>(objc_getClass("NSAlert")), sel_registerName("alloc")),
      sel_registerName("init"));
  reinterpret_cast<SendV>(objc_msgSend)(alert, sel_registerName("setMessageText:"),
                                        nsstr(o.message.empty() ? o.title : o.message));
  if (!o.detail.empty()) {
    reinterpret_cast<SendV>(objc_msgSend)(alert, sel_registerName("setInformativeText:"),
                                          nsstr(o.detail));
  }
  using SetStyle = void (*)(id, SEL, unsigned long);
  const unsigned long style = o.type == "error" ? 2UL : o.type == "warning" ? 0UL : 1UL;
  reinterpret_cast<SetStyle>(objc_msgSend)(alert, sel_registerName("setAlertStyle:"), style);
  for (const auto& b : buttons) {
    reinterpret_cast<SendV>(objc_msgSend)(alert, sel_registerName("addButtonWithTitle:"), nsstr(b));
  }
  const long r = reinterpret_cast<SendL>(objc_msgSend)(alert, sel_registerName("runModal"));
  reinterpret_cast<void (*)(id, SEL)>(objc_msgSend)(alert, sel_registerName("release"));
  const long index = r - 1000; // NSAlertFirstButtonReturn
  return index >= 0 && index < static_cast<long>(buttons.size()) ? static_cast<int>(index) : -1;

#else
  using namespace detail;
  SyncCtx ctx;
  ctx.loop = g_main_loop_new(nullptr, TRUE);
  GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", o.message.c_str());
  if (!o.detail.empty()) gtk_alert_dialog_set_detail(dlg, o.detail.c_str());
  std::vector<const char*> labels;
  labels.reserve(buttons.size() + 1);
  for (const auto& b : buttons) labels.push_back(b.c_str());
  labels.push_back(nullptr);
  gtk_alert_dialog_set_buttons(dlg, labels.data());
  gtk_alert_dialog_set_default_button(dlg, o.default_id);
  gtk_alert_dialog_set_modal(dlg, TRUE);
  gtk_alert_dialog_choose(dlg, parent ? GTK_WINDOW(parent) : nullptr, nullptr,
      +[](GObject* src, GAsyncResult* res, gpointer data) {
        auto* c = static_cast<SyncCtx*>(data);
        c->button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
        g_main_loop_quit(c->loop);
      }, &ctx);
  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);
  g_object_unref(dlg);
  return ctx.button;
#endif
}

} // namespace dialogs
