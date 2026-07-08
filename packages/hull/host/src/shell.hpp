#pragma once
// OS integration for link handling: open a URL with the OS default handler
// (browser / mail client / dialer) and spawn a detached child host window.
// Policy (which URLs are allowed, child argv) lives in url_policy.hpp so it
// stays unit-testable; this header is the side-effecting half.
//
// Test hook: HULL_SHELL_DRYRUN=1 makes both operations print what they WOULD
// do on stderr and report success, so e2e tests don't open real browsers.
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "url_policy.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>    // SHParseDisplayName / SHOpenFolderAndSelectItems (reveal)
#include <objbase.h>   // CoTaskMemFree
#elif defined(__APPLE__)
#include <mach-o/dyld.h>     // _NSGetExecutablePath
#include <objc/runtime.h>    // NSWorkspace via the objc runtime (same pattern as main.cpp)
#include <objc/message.h>
#include <unistd.h>          // fork/execv (child window spawn)
#include <sys/wait.h>
#else
#include <gio/gio.h>         // g_app_info_launch_default_for_uri
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace shell {

inline bool dryrun() { return std::getenv("HULL_SHELL_DRYRUN") != nullptr; }

// Absolute path of the running host binary (for spawning child windows).
inline std::string self_exe_path() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, &out[0], len, nullptr, nullptr);
  return out;
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(&buf[0], &size) != 0) return {};
  buf.resize(buf.find('\0') == std::string::npos ? buf.size() : buf.find('\0'));
  return buf;
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, (size_t)n) : std::string();
#endif
}

#if defined(_WIN32)
inline std::string narrow_utf16(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  s.resize(n - 1); // drop the trailing NUL
  return s;
}

inline std::wstring widen_utf8(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(n - 1); // drop the trailing NUL
  return w;
}

// Quote one argv element per the MSVCRT parsing rules (backslash-doubling before ").
inline std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') { backslashes++; out += c; }
    else if (c == L'"') { out.append(backslashes + 1, L'\\'); out += c; backslashes = 0; }
    else { backslashes = 0; out += c; }
  }
  out.append(backslashes, L'\\');
  out += L'"';
  return out;
}
#endif

// Launch `args` (argv[0] = executable) as a fully detached process.
inline bool spawn_detached(const std::vector<std::string>& args) {
  if (args.empty()) return false;
#if defined(_WIN32)
  std::wstring cmdline;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) cmdline += L' ';
    cmdline += quote_arg(widen_utf8(args[i]));
  }
  STARTUPINFOW si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring exe = widen_utf8(args[0]);
  if (!CreateProcessW(exe.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                      CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  // Double-fork so the grandchild is reparented to init — no zombies, and the
  // child window outlives (or dies independently of) this process. argv is
  // built BEFORE forking: between fork and exec only async-signal-safe calls
  // are allowed in a multithreaded process (no malloc).
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    const pid_t pid2 = fork();
    if (pid2 != 0) _exit(pid2 < 0 ? 1 : 0);
    setsid();
    execv(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// Open `url` with the OS default handler. Fails closed on disallowed schemes.
inline bool open_external(const std::string& url) {
  if (!url_policy::allowed_external_url(url)) return false;
  if (dryrun()) {
    std::cerr << "hull-host: [dry-run] open-external " << url << "\n";
    return true;
  }
#if defined(_WIN32)
  const std::wstring wurl = widen_utf8(url);
  return reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
  using Send0   = id (*)(id, SEL);
  using SendStr = id (*)(id, SEL, const char*);
  using SendId  = id (*)(id, SEL, id);
  using SendIdB = bool (*)(id, SEL, id);
  id nsString = reinterpret_cast<id>(objc_getClass("NSString"));
  id nsUrlCls = reinterpret_cast<id>(objc_getClass("NSURL"));
  id wsCls    = reinterpret_cast<id>(objc_getClass("NSWorkspace"));
  id str = reinterpret_cast<SendStr>(objc_msgSend)(
      nsString, sel_registerName("stringWithUTF8String:"), url.c_str());
  id nsurl = reinterpret_cast<SendId>(objc_msgSend)(
      nsUrlCls, sel_registerName("URLWithString:"), str);
  if (!nsurl) return false;
  id ws = reinterpret_cast<Send0>(objc_msgSend)(wsCls, sel_registerName("sharedWorkspace"));
  return reinterpret_cast<SendIdB>(objc_msgSend)(ws, sel_registerName("openURL:"), nsurl);
#else
  return g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr) == TRUE;
#endif
}

// Open a local file/folder with its OS default application.
inline bool open_path(const std::string& path) {
  if (path.empty()) return false;
  if (dryrun()) {
    std::cerr << "hull-host: [dry-run] open-path " << path << "\n";
    return true;
  }
#if defined(_WIN32)
  const std::wstring wpath = widen_utf8(path);
  return reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
  using Send0   = id (*)(id, SEL);
  using SendStr = id (*)(id, SEL, const char*);
  using SendId  = id (*)(id, SEL, id);
  using SendIdB = bool (*)(id, SEL, id);
  id nsPath = reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), path.c_str());
  id url = reinterpret_cast<SendId>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSURL")), sel_registerName("fileURLWithPath:"), nsPath);
  if (!url) return false;
  id ws = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSWorkspace")), sel_registerName("sharedWorkspace"));
  return reinterpret_cast<SendIdB>(objc_msgSend)(ws, sel_registerName("openURL:"), url);
#else
  gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  if (!uri) return false;
  const bool ok = g_app_info_launch_default_for_uri(uri, nullptr, nullptr) == TRUE;
  g_free(uri);
  return ok;
#endif
}

// Reveal a file in the OS file manager (Finder / Explorer / Files), selected.
inline bool reveal_path(const std::string& path) {
  if (path.empty()) return false;
  if (dryrun()) {
    std::cerr << "hull-host: [dry-run] reveal-path " << path << "\n";
    return true;
  }
#if defined(_WIN32)
  const std::wstring wpath = widen_utf8(path);
  PIDLIST_ABSOLUTE pidl = nullptr;
  if (FAILED(SHParseDisplayName(wpath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
  const bool ok = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
  CoTaskMemFree(pidl);
  return ok;
#elif defined(__APPLE__)
  using Send0   = id (*)(id, SEL);
  using SendStr = id (*)(id, SEL, const char*);
  using SendId  = id (*)(id, SEL, id);
  using SendV   = void (*)(id, SEL, id);
  id nsPath = reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), path.c_str());
  id url = reinterpret_cast<SendId>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSURL")), sel_registerName("fileURLWithPath:"), nsPath);
  if (!url) return false;
  id arr = reinterpret_cast<SendId>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSArray")), sel_registerName("arrayWithObject:"), url);
  id ws = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSWorkspace")), sel_registerName("sharedWorkspace"));
  reinterpret_cast<SendV>(objc_msgSend)(
      ws, sel_registerName("activateFileViewerSelectingURLs:"), arr);
  return true;
#else
  // Preferred: the FileManager1 D-Bus interface (selects the file). Fallback:
  // open the containing directory.
  gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  if (!uri) return false;
  bool ok = false;
  GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (conn) {
    GVariantBuilder items;
    g_variant_builder_init(&items, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&items, "s", uri);
    GVariant* ret = g_dbus_connection_call_sync(
        conn, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
        "org.freedesktop.FileManager1", "ShowItems",
        g_variant_new("(ass)", &items, ""),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
    if (ret) { g_variant_unref(ret); ok = true; }
    g_object_unref(conn);
  }
  g_free(uri);
  if (!ok) {
    const std::string parent = std::string(path, 0, path.find_last_of('/'));
    ok = open_path(parent.empty() ? "/" : parent);
  }
  return ok;
#endif
}

// Move a file/folder to the OS trash / recycle bin (reversible by the user).
inline bool trash_path(const std::string& path) {
  if (path.empty()) return false;
  if (dryrun()) {
    std::cerr << "hull-host: [dry-run] trash-path " << path << "\n";
    return true;
  }
#if defined(_WIN32)
  std::wstring wpath = widen_utf8(path);
  wpath.push_back(L'\0'); // SHFileOperation needs a double-NUL-terminated list
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = wpath.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
#elif defined(__APPLE__)
  using Send0   = id (*)(id, SEL);
  using SendStr = id (*)(id, SEL, const char*);
  using SendId  = id (*)(id, SEL, id);
  using Trash   = bool (*)(id, SEL, id, id, id);
  id nsPath = reinterpret_cast<SendStr>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSString")),
      sel_registerName("stringWithUTF8String:"), path.c_str());
  id url = reinterpret_cast<SendId>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSURL")), sel_registerName("fileURLWithPath:"), nsPath);
  if (!url) return false;
  id fm = reinterpret_cast<Send0>(objc_msgSend)(
      reinterpret_cast<id>(objc_getClass("NSFileManager")), sel_registerName("defaultManager"));
  return reinterpret_cast<Trash>(objc_msgSend)(
      fm, sel_registerName("trashItemAtURL:resultingItemURL:error:"), url, nullptr, nullptr);
#else
  GFile* f = g_file_new_for_path(path.c_str());
  const bool ok = g_file_trash(f, nullptr, nullptr) == TRUE;
  g_object_unref(f);
  return ok;
#endif
}

// Open `url` (web content only) in a NEW Hull window: a detached child host
// process with --no-bridge, so the remote page never sees the native bindings.
inline bool open_window(const std::string& url, const std::string& title,
                        int width, int height) {
  if (!url_policy::allowed_window_url(url)) return false;
  const std::string exe = self_exe_path();
  if (exe.empty()) return false;
  const auto args = url_policy::window_args(exe, url, title, width, height);
  if (dryrun()) {
    std::cerr << "hull-host: [dry-run] open-window";
    for (const auto& a : args) std::cerr << ' ' << a;
    std::cerr << "\n";
    return true;
  }
  return spawn_detached(args);
}

} // namespace shell
