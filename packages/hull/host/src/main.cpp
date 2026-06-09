// Hull host — a single, generic native web-view runtime.
//
// Modes:
//   (window)  --url <devUrl> | --app <file.html>   render the UI in an OS web view
//   (serve)   --serve <port>                       headless HTTP/SSE bridge (browser dev mode)
//   --title --width --height --app-id --debug --inspect
//
// Bindings register into a transport-agnostic Dispatcher, then are exposed either over
// the web view (window mode) or over HTTP/SSE (serve mode). --inspect turns on the dev
// trace (mirrors every call/reply/event on the "__trace" event for the inspector).

// Must precede any libc header so glibc exposes unshare()/CLONE_NEWUSER (Linux sandbox
// probe). g++ defines this by default; the guard just makes it portable + warning-free.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <optional>
#include <thread>
#include <nlohmann/json.hpp>

#include "dispatcher.hpp"
#include "secure.hpp"
#include "bindings/http.hpp"   // pulls httplib first (Linux X11 macro clash)
#include "serve.hpp"           // httplib HTTP/SSE bridge server
#include "webview/webview.h"
#include "bindings/printer.hpp"
#include "bindings/storage.hpp"
#include "bindings/credentials.hpp"
#include "bindings/database.hpp"
#include "bindings/files.hpp"

#ifdef _WIN32
#include <windows.h>
#include <gdiplus.h>
#endif

#if defined(__linux__)
#include <sched.h>      // unshare, CLONE_NEWUSER
#include <sys/wait.h>   // waitpid
#include <unistd.h>     // fork, getuid, write, close, readlink
#include <fcntl.h>      // open
#include <cstdio>       // snprintf
#include <cstdlib>      // setenv / getenv / system
#include <filesystem>   // file:// URL + desktop-integration paths
#include <glib.h>       // g_set_prgname (window app-id -> desktop icon)
#endif

using json = nlohmann::json;

namespace {

// Set the window icon at runtime from an image file (the host is generic/prebuilt, so
// the icon can't be embedded at compile time). Windows: GDI+ loads PNG/ICO into an
// HICON. macOS/Linux(GTK4) window icons come from the app bundle / .desktop file, so
// this is a no-op there (documented).
#if defined(_WIN32)
void set_window_icon(webview::webview& w, const std::string& path) {
  auto win = w.window();          // webview 0.12: result<void*>
  if (!win.ok()) return;
  HWND hwnd = static_cast<HWND>(win.value());
  if (!hwnd) return;
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 0) return;
  std::wstring wpath(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], n);
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartupInput gsi;
  if (Gdiplus::GdiplusStartup(&token, &gsi, nullptr) != Gdiplus::Ok) return;
  Gdiplus::Bitmap bmp(wpath.c_str());
  if (bmp.GetLastStatus() == Gdiplus::Ok) {
    HICON hIcon = nullptr;
    if (bmp.GetHICON(&hIcon) == Gdiplus::Ok && hIcon) {
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
      // hIcon intentionally kept for the process lifetime (the window owns it).
    }
  }
  // GDI+ left initialized so the icon stays valid.
}
#else
void set_window_icon(webview::webview&, const std::string&) { /* set via app bundle / .desktop */ }
#endif

#if defined(__linux__)
// WebKitGTK isolates its web/network subprocesses with bubblewrap, which needs
// unprivileged user namespaces. When those are blocked — Ubuntu 24.04 enables
// kernel.apparmor_restrict_unprivileged_userns by default, and many containers lack
// them — the web process aborts with "bwrap: setting up uid map: Permission denied".
//
// Probe in a child process (so the parent's namespaces are untouched) whether bwrap's
// rootless setup will work: create a user namespace AND write its uid map. The map
// write is the step that actually fails on Ubuntu 24.04 — its
// apparmor_restrict_unprivileged_userns lets unshare(CLONE_NEWUSER) succeed but denies
// writing /proc/self/uid_map ("bwrap: setting up uid map: Permission denied"), so a
// probe that only tries unshare reports a false positive. If this fails, disable the
// WebKitGTK sandbox so the app still runs — unless the user made an explicit choice:
//   WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS set  -> respect it (CLI/launcher opt-out)
//   HULL_FORCE_SANDBOX set                         -> keep the sandbox, never auto-disable
bool userns_available() {
  pid_t pid = fork();
  if (pid < 0) return true;                  // can't probe — assume the sandbox works
  if (pid == 0) {
    if (unshare(CLONE_NEWUSER) != 0) _exit(1);
    int fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0) _exit(1);
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "0 %d 1\n", (int)getuid());
    ssize_t w = write(fd, buf, (size_t)n);   // EPERM here => bwrap would fail too
    close(fd);
    _exit(w == (ssize_t)n ? 0 : 1);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return true;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void maybe_disable_webkit_sandbox() {
  if (std::getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) return; // explicit opt-out
  if (std::getenv("HULL_FORCE_SANDBOX")) return;                       // forced on
  if (userns_available()) return;                                     // sandbox will work
  setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
  std::cerr << "hull: unprivileged user namespaces are unavailable; disabling the "
               "WebKitGTK sandbox so the app can run.\n"
               "      To keep the sandbox, enable userns (e.g. sudo sysctl "
               "kernel.apparmor_restrict_unprivileged_userns=0) or set "
               "HULL_FORCE_SANDBOX=1.\n";
}

// GTK4 has no runtime "set window icon from a PNG" (unlike Windows GDI+): the title
// bar / dock / Alt-Tab icon is drawn by the compositor from a .desktop file matched to
// the window's app-id. So to show an icon on Linux we (1) install the PNG into the user
// icon theme, (2) write a .desktop whose Icon points at it, and (3) set the window's
// app-id (via g_set_prgname, before GTK init) to that .desktop's id so they're matched.
// Works on both Wayland and X11. Idempotent; writes only under XDG data home.
void install_desktop_integration(const std::string& app_id, const std::string& title,
                                 const std::string& icon_path) {
  if (app_id.empty()) return;
  std::error_code ec;
  const char* home = std::getenv("HOME");
  const char* xdg = std::getenv("XDG_DATA_HOME");
  std::filesystem::path data = (xdg && *xdg) ? std::filesystem::path(xdg)
      : std::filesystem::path(home ? home : ".") / ".local" / "share";

  // Detect an installed binary (e.g. from a .deb under /opt or /usr). The package
  // already ships a *visible* /usr/share/applications/<app-id>.desktop + icon, so we must
  // NOT write a user-level NoDisplay entry — a ~/.local one overrides the system file and
  // would hide the app from the menu. Set the app-id (for window<->.desktop matching) and
  // remove any stale dev-created user entry so the installed app shows up.
  {
    char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = n > 0 ? std::string(buf, (size_t)n) : "";
    if (p.rfind("/usr/", 0) == 0 || p.rfind("/opt/", 0) == 0) {
      const std::filesystem::path stale = data / "applications" / (app_id + ".desktop");
      std::ifstream in(stale);
      std::string c((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      if (c.find("X-Hull-Generated=true") != std::string::npos) std::filesystem::remove(stale, ec);
      g_set_prgname(app_id.c_str());
      return;
    }
  }

  // (1) icon -> ~/.local/share/icons/hicolor/256x256/apps/<app-id>.png
  if (!icon_path.empty() && std::filesystem::exists(icon_path, ec)) {
    auto icondir = data / "icons" / "hicolor" / "256x256" / "apps";
    std::filesystem::create_directories(icondir, ec);
    std::filesystem::copy_file(icon_path, icondir / (app_id + ".png"),
        std::filesystem::copy_options::overwrite_existing, ec);
  }

  // (2) ~/.local/share/applications/<app-id>.desktop
  auto appsdir = data / "applications";
  std::filesystem::create_directories(appsdir, ec);
  std::string exe;
  { char buf[4096]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; exe = buf; } }
  std::ofstream f(appsdir / (app_id + ".desktop"), std::ios::trunc);
  if (f) {
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=" << (title.empty() ? app_id : title) << "\n"
      << "Icon=" << app_id << "\n"
      << "Exec=" << (exe.empty() ? app_id : exe) << "\n"
      << "StartupWMClass=" << app_id << "\n"
      << "NoDisplay=true\n"          // matched for the running window, hidden from menus
      << "X-Hull-Generated=true\n";
  }

  // (3) match the running window to that .desktop via the Wayland/X11 app-id.
  g_set_prgname(app_id.c_str());
}

// Build a percent-encoded file:// URL from a local path. On WebKitGTK we load the
// packaged app.html by URL (not set_html): set_html gives the document a null base
// URL, under which <script type="module"> — what the single-file bundle uses — does
// not execute, so the app renders blank. A real file:// origin runs the modules.
std::string file_uri(const std::string& path) {
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(path, ec);
  const std::string p = ec ? path : abs.string();
  static const char* hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : p) {
    if (c == '/' || c == '-' || c == '_' || c == '.' || c == '~' ||
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += static_cast<char>(c);
    } else {
      out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F];
    }
  }
  return out;
}
#endif

struct Options {
  std::optional<std::string> url;
  std::optional<std::string> app;
  std::optional<int> serve_port;
  std::optional<int> inspect_port;
  std::optional<std::string> icon;
  std::string title = "Hull App";
  std::string appId = "Hull";
  int width = 1100;
  int height = 760;
  bool debug = false;
  bool inspect = false;
};

std::optional<std::string> next_arg(int argc, char** argv, int& i) {
  if (i + 1 < argc) return std::string(argv[++i]);
  return std::nullopt;
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--url")         { if (auto v = next_arg(argc, argv, i)) o.url = *v; }
    else if (a == "--app")    { if (auto v = next_arg(argc, argv, i)) o.app = *v; }
    else if (a == "--serve")  { if (auto v = next_arg(argc, argv, i)) o.serve_port = std::stoi(*v); }
    else if (a == "--inspect-port") { if (auto v = next_arg(argc, argv, i)) o.inspect_port = std::stoi(*v); }
    else if (a == "--title")  { if (auto v = next_arg(argc, argv, i)) o.title = *v; }
    else if (a == "--app-id") { if (auto v = next_arg(argc, argv, i)) o.appId = *v; }
    else if (a == "--icon")   { if (auto v = next_arg(argc, argv, i)) o.icon = *v; }
    else if (a == "--width")  { if (auto v = next_arg(argc, argv, i)) o.width = std::stoi(*v); }
    else if (a == "--height") { if (auto v = next_arg(argc, argv, i)) o.height = std::stoi(*v); }
    else if (a == "--debug")  { o.debug = true; }
    else if (a == "--inspect") { o.inspect = true; }
  }
  return o;
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

const char* FALLBACK_HTML =
  "<!doctype html><html><head><meta charset='utf-8'><title>Hull</title>"
  "<style>html,body{margin:0;height:100%;font-family:system-ui,sans-serif;"
  "display:flex;align-items:center;justify-content:center;background:#0f172a;color:#e2e8f0}"
  ".c{max-width:34rem;padding:2rem;text-align:center}h1{font-size:1.5rem;margin:.2rem 0}"
  "code{background:#1e293b;padding:.15rem .4rem;border-radius:.3rem;color:#93c5fd}"
  "p{color:#94a3b8;line-height:1.5}</style></head><body><div class='c'>"
  "<h1>\xE2\x9B\xB5 Hull</h1>"
  "<p>No app was provided. Run <code>hull dev</code> during development, "
  "or <code>hull build</code> to package your UI.</p></div></body></html>";

void register_all(Dispatcher& d, const Options& opt) {
  d.on("ping", [](const json& a, Reply reply) {
    reply(json{{"ok", true}, {"echo", a.empty() ? json(nullptr) : a.at(0)}});
  });
  d.on("appInfo", [opt](const json&, Reply reply) {
    reply(json{{"ok", true}, {"appId", opt.appId}, {"secure", secure::active()}});
  });
  register_http_bindings(d);
  register_printer_bindings(d);
  register_storage_bindings(d);
  register_credentials_bindings(d);
  register_database_bindings(d);
  register_files_bindings(d);
}

} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  int argc = __argc;
  char** argv = __argv;
#else
int main(int argc, char** argv) {
#endif
  Options opt = parse_args(argc, argv);
  storage::set_app_name(opt.appId);

  Dispatcher d;
  if (opt.inspect) d.set_trace(true);
  register_all(d, opt);

  // ---- Serve mode: headless HTTP/SSE bridge (browser dev mode) ----
  if (opt.serve_port) {
    try {
      BridgeServer server(d);
      d.set_emit_sink([&server](const std::string& e, const json& p) { server.broadcast(e, p); });
      server.listen("127.0.0.1", *opt.serve_port); // blocks
    } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
    }
    return 0;
  }

  // ---- Window mode: render in the OS web view ----
  try {
#if defined(__linux__)
    maybe_disable_webkit_sandbox();   // before any GTK/WebKit init (fork is safe here)
    if (opt.icon) install_desktop_integration(opt.appId, opt.title, *opt.icon);
#endif
    webview::webview window(opt.debug, nullptr);
    window.set_title(opt.title);
    window.set_size(opt.width, opt.height, WEBVIEW_HINT_NONE);
    if (opt.icon) set_window_icon(window, *opt.icon);

    // Optional trace server: lets the inspector (a browser tab) observe this native
    // app's bridge activity. Leaked intentionally — lives for the whole process.
    BridgeServer* trace = opt.inspect_port ? new BridgeServer(d) : nullptr;

    // emit -> push into the page; also mirror to the inspector trace server if on.
    d.set_emit_sink([&window, trace](const std::string& event, const json& payload) {
      const std::string js =
          "if(window.__bridgeEmit){window.__bridgeEmit(" +
          json(event).dump() + "," + json(payload.dump()).dump() + ");}";
      window.dispatch([&window, js] { window.eval(js); });
      if (trace) trace->broadcast(event, payload);
    });

    if (trace) {
      const int port = *opt.inspect_port;
      std::thread([trace, port] { trace->listen("127.0.0.1", port); }).detach();
    }

    // bind every dispatcher handler onto window.<name>
    for (const auto& name : d.names()) {
      window.bind(
          name,
          [&d, &window, name](const std::string& id, const std::string& args_str, void*) {
            json args;
            try { args = json::parse(args_str); } catch (...) { args = json::array(); }
            d.invoke(name, args, [&window, id](const json& res) {
              window.resolve(id, 0, res.dump());
            });
          },
          nullptr);
    }

    if (opt.url) {
      window.navigate(*opt.url);
    } else if (opt.app) {
#if defined(__linux__)
      // Load by file:// URL so module scripts run (set_html's null base blocks them
      // on WebKitGTK). Fall back to set_html only if the file is missing.
      if (std::filesystem::exists(*opt.app)) window.navigate(file_uri(*opt.app));
      else window.set_html(FALLBACK_HTML);
#else
      if (auto html = read_file(*opt.app)) window.set_html(*html);
      else window.set_html(FALLBACK_HTML);
#endif
    } else {
      window.set_html(FALLBACK_HTML);
    }

    window.run();
  } catch (const webview::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
