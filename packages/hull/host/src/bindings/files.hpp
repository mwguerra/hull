#pragma once
// File storage bindings (uploads/blobs). Logic lives in file_store.hpp (webview-free
// + unit-tested); these run it on a worker thread and speak base64 over the bridge.
#include <thread>
#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"
#include "../file_store.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

inline void register_files_bindings(Dispatcher& d) {
  // fileWrite(name, base64) -> { ok }
  d.on("fileWrite", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        appfiles::write_file(a.at(0).get<std::string>(),
                             appfiles::b64decode(a.at(1).get<std::string>()));
        reply(json{{"ok", true}});
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });

  // fileRead(name) -> { ok, data: base64 }
  d.on("fileRead", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        reply(json{{"ok", true}, {"data", appfiles::b64encode(appfiles::read_file(a.at(0).get<std::string>()))}});
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });

  // fileList() -> { ok, files: [{name, size}] }
  d.on("fileList", [](const json&, Reply reply) {
    std::thread([reply]() {
      try {
        json arr = json::array();
        for (const auto& e : fs::directory_iterator(appfiles::dir())) {
          if (!e.is_regular_file()) continue;
          arr.push_back({{"name", e.path().filename().string()}, {"size", (int64_t)e.file_size()}});
        }
        reply(json{{"ok", true}, {"files", arr}});
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });

  // fileDelete(name) -> { ok, removed }
  d.on("fileDelete", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        bool removed = fs::remove(appfiles::dir() / appfiles::safe_name(a.at(0).get<std::string>()));
        reply(json{{"ok", true}, {"removed", removed}});
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });
}
