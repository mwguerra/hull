#pragma once
// SQLite core for Hull: a small, safe, parameterized SQL surface returning JSON.
// Webview-free on purpose (only paths.hpp + sqlite3 + json) so it can be unit-tested
// standalone and reused by the bridge bindings (database.hpp).
//
// Security/speed posture:
//  - one statement per exec/query/get (prepare_v2 ignores trailing SQL) -> no stacked
//    "...; DROP TABLE" injection; multi-statement only via batch([...]) arrays
//  - parameters are bound, never concatenated (sqlite3_bind_*)
//  - DB lives in the per-user app dir, chmod 0600 on POSIX
//  - WAL + NORMAL sync + busy_timeout for speed; foreign_keys + trusted_schema=OFF
#include <string>
#include <mutex>
#include <stdexcept>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "paths.hpp"
#include "secure.hpp"   // active() + the per-install key (secure build only)
#include "sqlite3.h"    // vendored SQLite by default; SQLCipher's header in the secure build

using json = nlohmann::json;

namespace hulldb {

// Optional path override (used by tests). Default: <app_data_dir>/app.db
inline std::string& path_override() { static std::string p; return p; }
inline void set_db_path(const std::string& p) { path_override() = p; }

inline std::mutex& mtx() { static std::mutex m; return m; }

inline std::string db_file() {
  if (!path_override().empty()) return path_override();
  return (storage::app_data_dir() / "app.db").string();
}

// Lazily open the single connection and apply pragmas. Caller must hold mtx().
inline sqlite3* handle() {
  static sqlite3* db = nullptr;
  if (db) return db;
  const std::string path = db_file();
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    std::string err = db ? sqlite3_errmsg(db) : "open failed";
    if (db) { sqlite3_close(db); db = nullptr; }
    throw std::runtime_error("sqlite open: " + err);
  }
  storage::lock_down(path); // 0600 on POSIX
#if defined(HULL_CRYPTO)
  // SQLCipher: set the encryption key (raw 32-byte key from the keychain, hex) as the
  // very first operation, before any other SQL. The DB file is AES-encrypted at rest.
  {
    auto k = secure::data_key();
    static const char* H = "0123456789abcdef";
    std::string hex;
    hex.reserve(k.size() * 2);
    for (unsigned char c : k) { hex.push_back(H[c >> 4]); hex.push_back(H[c & 0xF]); }
    const std::string pragma = "PRAGMA key = \"x'" + hex + "'\";";
    sqlite3_exec(db, pragma.c_str(), nullptr, nullptr, nullptr);
  }
#endif
  for (const char* p : {
         "PRAGMA journal_mode=WAL;",
         "PRAGMA synchronous=NORMAL;",
         "PRAGMA foreign_keys=ON;",
         "PRAGMA busy_timeout=5000;",
         "PRAGMA trusted_schema=OFF;",
       }) {
    sqlite3_exec(db, p, nullptr, nullptr, nullptr);
  }
  return db;
}

inline std::string b64(const unsigned char* data, int len) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (int i = 0; i < len; i += 3) {
    int n = data[i] << 16;
    if (i + 1 < len) n |= data[i + 1] << 8;
    if (i + 2 < len) n |= data[i + 2];
    out.push_back(T[(n >> 18) & 63]);
    out.push_back(T[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? T[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? T[n & 63] : '=');
  }
  return out;
}

inline void bind_value(sqlite3_stmt* st, int i, const json& v) {
  if (v.is_null()) sqlite3_bind_null(st, i);
  else if (v.is_boolean()) sqlite3_bind_int(st, i, v.get<bool>() ? 1 : 0);
  else if (v.is_number_integer()) sqlite3_bind_int64(st, i, v.get<int64_t>());
  else if (v.is_number_unsigned()) sqlite3_bind_int64(st, i, (int64_t)v.get<uint64_t>());
  else if (v.is_number_float()) sqlite3_bind_double(st, i, v.get<double>());
  else if (v.is_string()) {
    const std::string s = v.get<std::string>();
    sqlite3_bind_text(st, i, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
  } else {
    throw std::runtime_error(
        "unsupported parameter type — use null/boolean/number/string "
        "(JSON.stringify objects/arrays into a TEXT column)");
  }
}

inline void bind_all(sqlite3_stmt* st, const json& params) {
  if (params.is_null()) return;
  if (!params.is_array()) throw std::runtime_error("params must be an array");
  for (int i = 0; i < (int)params.size(); ++i) bind_value(st, i + 1, params[i]);
}

inline json row_to_json(sqlite3_stmt* st) {
  json row = json::object();
  const int cols = sqlite3_column_count(st);
  for (int c = 0; c < cols; ++c) {
    const char* name = sqlite3_column_name(st, c);
    switch (sqlite3_column_type(st, c)) {
      case SQLITE_INTEGER: row[name] = (int64_t)sqlite3_column_int64(st, c); break;
      case SQLITE_FLOAT:   row[name] = sqlite3_column_double(st, c); break;
      case SQLITE_TEXT:
        row[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, c)),
                                sqlite3_column_bytes(st, c));
        break;
      case SQLITE_BLOB:
        row[name] = b64(reinterpret_cast<const unsigned char*>(sqlite3_column_blob(st, c)),
                        sqlite3_column_bytes(st, c));
        break;
      default: row[name] = nullptr; break; // SQLITE_NULL
    }
  }
  return row;
}

// Run ONE statement (caller holds mtx). Returns { rows, changes, lastInsertRowid }.
inline json run_one(sqlite3* db, const std::string& sql, const json& params) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(db));
  }
  bind_all(st, params);
  json rows = json::array();
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) rows.push_back(row_to_json(st));
  if (rc != SQLITE_DONE) {
    std::string e = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    throw std::runtime_error(e);
  }
  sqlite3_finalize(st);
  return json{{"rows", rows},
              {"changes", sqlite3_changes(db)},
              {"lastInsertRowid", (int64_t)sqlite3_last_insert_rowid(db)}};
}

// ---- Public API (each throws std::runtime_error on failure) ----

inline json exec(const std::string& sql, const json& params) {
  std::lock_guard<std::mutex> lock(mtx());
  json r = run_one(handle(), sql, params);
  return json{{"ok", true}, {"changes", r["changes"]}, {"lastInsertRowid", r["lastInsertRowid"]}};
}

inline json query(const std::string& sql, const json& params) {
  std::lock_guard<std::mutex> lock(mtx());
  json r = run_one(handle(), sql, params);
  return json{{"ok", true}, {"rows", r["rows"]}};
}

inline json get(const std::string& sql, const json& params) {
  std::lock_guard<std::mutex> lock(mtx());
  json r = run_one(handle(), sql, params);
  return json{{"ok", true}, {"row", r["rows"].empty() ? json(nullptr) : r["rows"][0]}};
}

// Run several statements atomically (one transaction; rollback on any error).
inline json batch(const json& statements) {
  if (!statements.is_array()) throw std::runtime_error("batch expects an array of { sql, params }");
  std::lock_guard<std::mutex> lock(mtx());
  sqlite3* db = handle();
  run_one(db, "BEGIN IMMEDIATE;", json(nullptr));
  try {
    json results = json::array();
    for (const auto& s : statements) {
      const std::string sql = s.at("sql").get<std::string>();
      const json params = s.contains("params") ? s["params"] : json::array();
      json r = run_one(db, sql, params);
      results.push_back({{"changes", r["changes"]},
                         {"lastInsertRowid", r["lastInsertRowid"]},
                         {"rows", r["rows"]}});
    }
    run_one(db, "COMMIT;", json(nullptr));
    return json{{"ok", true}, {"results", results}};
  } catch (...) {
    try { run_one(db, "ROLLBACK;", json(nullptr)); } catch (...) {}
    throw;
  }
}

} // namespace hulldb
