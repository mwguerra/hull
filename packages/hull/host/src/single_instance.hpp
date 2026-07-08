#pragma once
// Opt-in single-instance lock (.hullrc singleInstance: true -> --single-instance).
//
// The PRIMARY instance holds an exclusive OS lock on <app data dir>/instance.lock
// and runs a tiny loopback HTTP listener whose port is written to instance.port.
// A SECOND instance fails to take the lock, POSTs /present to that port, and
// exits — the primary presents (focuses) its window and emits the
// "app:second-instance" event to the app.
//
// The lock is an OS handle, so it dies with the process — no stale-lock cleanup
// needed (the port file may go stale, but it's only read after a failed lock).
#include <httplib.h>   // before any webview/GTK/X11 headers

#include <atomic>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "paths.hpp"
#include "emit_hook.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace single_instance {

inline std::function<void()>& present_handler() {
  static std::function<void()> fn;
  return fn;
}

// Set false the instant window.run() returns, so a /present arriving during
// shutdown never dispatches into the destructing window (the listener thread is
// detached and process-lifetime). Checked before touching the window.
inline std::atomic<bool>& active() {
  static std::atomic<bool> a{true};
  return a;
}

namespace detail {
inline fs::path lock_file() { return storage::app_data_dir() / "instance.lock"; }
inline fs::path port_file() { return storage::app_data_dir() / "instance.port"; }

inline bool try_lock() {
#if defined(_WIN32)
  // Exclusive open: a second CreateFileW fails with a sharing violation.
  // Handle intentionally leaked — it must live for the whole process.
  HANDLE h = CreateFileW(lock_file().wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  return h != INVALID_HANDLE_VALUE;
#else
  const int fd = ::open(lock_file().string().c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) return true; // can't probe — don't block the launch
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return false; }
  return true; // fd intentionally kept open for the process lifetime
#endif
}

inline void notify_primary() {
  std::ifstream in(port_file());
  int port = 0;
  in >> port;
  if (port <= 0) return;
  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(1);
  cli.set_read_timeout(2);
  cli.Post("/present", "", "text/plain");
}
}

// Returns true when this process is the primary instance (and starts the
// listener); false when another instance already runs (it has been notified —
// the caller should exit).
inline bool acquire() {
  if (!detail::try_lock()) {
    detail::notify_primary();
    return false;
  }
  auto* server = new httplib::Server(); // process-lifetime
  server->Post("/present", [](const httplib::Request&, httplib::Response& res) {
    if (active()) {
      if (present_handler()) present_handler();
      hooks::emit("app:second-instance", nlohmann::json::object());
    }
    res.set_content("ok", "text/plain");
  });
  const int port = server->bind_to_any_port("127.0.0.1");
  if (port > 0) {
    std::ofstream out(detail::port_file(), std::ios::trunc);
    out << port;
    out.close();
    storage::lock_down(detail::port_file());
    std::thread([server] { server->listen_after_bind(); }).detach();
  }
  return true;
}

} // namespace single_instance
