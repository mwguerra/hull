#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"
#include "paths.hpp"    // storage::app_data_dir / lock_down
#include "secure.hpp"   // at-rest crypto layer (NullCipher by default)

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace storage {

// ---- Key/value settings store (passed through the secure layer at rest) ----
inline fs::path settings_path() { return app_data_dir() / "settings.dat"; }

inline json read_settings() {
  std::ifstream f(settings_path(), std::ios::binary);
  if (!f) return json::object();
  std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (blob.empty()) return json::object();
  try {
    auto plain = secure::decrypt(blob);
    if (!plain) return json::object();
    return json::parse(*plain);
  } catch (...) {
    return json::object();
  }
}

inline void write_settings(const json& j) {
  const std::string blob = secure::encrypt(j.dump());
  fs::path tmp = settings_path();
  tmp += ".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(blob.data(), (std::streamsize)blob.size()); }
  fs::rename(tmp, settings_path()); // atomic replace
  lock_down(settings_path());
}

} // namespace storage

inline void register_storage_bindings(Dispatcher& d) {
  d.on("saveSetting", [&d](const json& a, Reply reply) {
    try {
      auto key = a.at(0).get<std::string>();
      json s = storage::read_settings();
      s[key] = a.at(1);
      storage::write_settings(s);
      d.emit("settings:changed", {{"key", key}, {"value", a.at(1)}});  // C++ -> UI
      reply(json{{"ok", true}});
    } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
  });

  d.on("loadSetting", [](const json& a, Reply reply) {
    try {
      json s = storage::read_settings();
      auto key = a.at(0).get<std::string>();
      reply(json{{"ok", true}, {"value", s.contains(key) ? s[key] : json(nullptr)}});
    } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
  });

  d.on("loadAllSettings", [](const json&, Reply reply) {
    try {
      reply(json{{"ok", true}, {"value", storage::read_settings()}});
    } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
  });
}
