#pragma once
// SQLite bridge bindings. Logic lives in db_core.hpp (webview-free + unit-tested);
// these run it on a worker thread and reply with the JSON result.
#include <thread>
#include <string>
#include <nlohmann/json.hpp>
#include "dispatcher.hpp"
#include "../db_core.hpp"

using json = nlohmann::json;

inline void register_database_bindings(Dispatcher& d) {
  // dbExec(sql, params?) -> { ok, changes, lastInsertRowid }
  d.on("dbExec", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        json params = a.size() > 1 ? a.at(1) : json::array();
        reply(hulldb::exec(a.at(0).get<std::string>(), params));
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });

  // dbQuery(sql, params?) -> { ok, rows }
  d.on("dbQuery", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        json params = a.size() > 1 ? a.at(1) : json::array();
        reply(hulldb::query(a.at(0).get<std::string>(), params));
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });

  // dbGet(sql, params?) -> { ok, row|null }
  d.on("dbGet", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        json params = a.size() > 1 ? a.at(1) : json::array();
        reply(hulldb::get(a.at(0).get<std::string>(), params));
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });

  // dbBatch([{sql, params?}, ...]) -> { ok, results }  (atomic transaction)
  d.on("dbBatch", [](const json& a, Reply reply) {
    std::thread([a, reply]() {
      try {
        reply(hulldb::batch(a.at(0)));
      } catch (const std::exception& e) { reply(json{{"ok", false}, {"error", e.what()}}); }
    }).detach();
  });
}
