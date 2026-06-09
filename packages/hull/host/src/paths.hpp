#pragma once
// App identity + per-user storage location. Deliberately free of webview/OpenSSL so
// it can be shared by storage.hpp (encryption) and db_core.hpp (SQLite), and unit-
// tested standalone.
#include <string>
#include <filesystem>
#include <system_error>
#include <cstdlib>

namespace fs = std::filesystem;

namespace storage {

// App identity: namespaces the per-user data dir AND the keychain entries so two
// Hull apps never clash. Set once at startup from --app-id (see host main.cpp).
inline std::string& app_name() {
  static std::string n = "Hull";
  return n;
}
inline void set_app_name(const std::string& n) {
  if (!n.empty()) app_name() = n;
}

// ---- Resolve and create the per-user data directory ----
inline fs::path app_data_dir() {
  fs::path base;
#if defined(_WIN32)
  if (const char* p = std::getenv("LOCALAPPDATA")) base = p;
  else base = fs::temp_directory_path();
#elif defined(__APPLE__)
  base = fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".")
       / "Library" / "Application Support";
#else
  if (const char* x = std::getenv("XDG_DATA_HOME")) base = x;
  else base = fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".local" / "share";
#endif
  fs::path dir = base / app_name();
  fs::create_directories(dir);
#if !defined(_WIN32)
  // Owner-only directory (0700). On Windows, %LOCALAPPDATA% is already per-user.
  fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
#endif
  return dir;
}

// ---- Restrict a path to the owner (POSIX). Files -> 0600; directories -> 0700,
// because a directory needs the execute (search) bit to create/rename/read entries
// inside it (without it, writes into the dir fail with EACCES). ----
inline void lock_down(const fs::path& p) {
#if !defined(_WIN32)
  std::error_code ec;
  if (fs::exists(p, ec)) {
    const fs::perms perms = fs::is_directory(p, ec)
        ? fs::perms::owner_all                                  // 0700 (rwx) for dirs
        : (fs::perms::owner_read | fs::perms::owner_write);     // 0600 (rw-) for files
    fs::permissions(p, perms, fs::perm_options::replace, ec);
  }
#endif
  (void)p;
}

} // namespace storage
