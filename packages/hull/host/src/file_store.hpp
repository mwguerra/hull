#pragma once
// File storage core (webview-free, so it's unit-testable). Files live under
// <app_data_dir>/files, named by a sanitized basename (no path traversal). Contents
// pass through the secure layer: plaintext by default, AES-256-GCM in the secure build.
#include <string>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include "paths.hpp"
#include "secure.hpp"

namespace fs = std::filesystem;

namespace appfiles {

inline fs::path dir() {
  fs::path d = storage::app_data_dir() / "files";
  fs::create_directories(d);
  storage::lock_down(d);
  return d;
}

// Reject anything that isn't a plain filename (no separators, "..", drive, etc.).
inline std::string safe_name(const std::string& name) {
  fs::path p(name);
  if (name.empty() || p.filename().string() != name || name == "." || name == "..") {
    throw std::runtime_error("invalid file name (use a plain name, no path separators)");
  }
  return name;
}

inline std::string b64encode(const std::string& in) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  const auto* d = reinterpret_cast<const unsigned char*>(in.data());
  for (size_t i = 0; i < in.size(); i += 3) {
    int n = d[i] << 16;
    if (i + 1 < in.size()) n |= d[i + 1] << 8;
    if (i + 2 < in.size()) n |= d[i + 2];
    out.push_back(T[(n >> 18) & 63]);
    out.push_back(T[(n >> 12) & 63]);
    out.push_back(i + 1 < in.size() ? T[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < in.size() ? T[n & 63] : '=');
  }
  return out;
}

inline std::string b64decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int buf = 0, bits = 0;
  for (char c : in) {
    int v = val(c);
    if (v < 0) continue; // skip '=', whitespace, etc.
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((char)((buf >> bits) & 0xFF)); }
  }
  return out;
}

inline void write_file(const std::string& name, const std::string& bytes) {
  const fs::path target = dir() / safe_name(name);
  const std::string blob = secure::encrypt(bytes);
  fs::path tmp = target; tmp += ".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(blob.data(), (std::streamsize)blob.size()); }
  fs::rename(tmp, target);
  storage::lock_down(target);
}

inline std::string read_file(const std::string& name) {
  const fs::path target = dir() / safe_name(name);
  std::ifstream f(target, std::ios::binary);
  if (!f) throw std::runtime_error("file not found: " + name);
  std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto plain = secure::decrypt(blob);
  if (!plain) throw std::runtime_error("could not read file (wrong key or corrupt): " + name);
  return *plain;
}

} // namespace appfiles
