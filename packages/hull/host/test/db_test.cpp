// Standalone functional test for the SQLite core (db_core.hpp). Webview-free, so it
// links just sqlite3 + nlohmann/json. Run via the host's CMake test or directly:
//   g++ -std=c++17 -I../src -I../third_party/sqlite db_test.cpp ../third_party/sqlite/sqlite3.c -lpthread -ldl -o dbtest && ./dbtest
#include <cstdio>
#include <cstdlib>
#include <string>
#include "db_core.hpp"

static int failures = 0;
static void check(bool cond, const char* what) {
  std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failures++;
}

int main() {
  // Use a throwaway file path (also exercises file open + lock_down).
  const std::string path = (fs::temp_directory_path() / "hull_db_test.db").string();
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
  hulldb::set_db_path(path);

  try {
    // migrate-style schema via batch (atomic)
    hulldb::batch(json::array({
      {{"sql", "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL, pinned INTEGER, score REAL)"}},
      {{"sql", "PRAGMA user_version = 1"}},
    }));
    json uv = hulldb::get("PRAGMA user_version", json::array());
    check(uv["row"]["user_version"] == 1, "migration set user_version");

    // parameterized insert with mixed types
    json e1 = hulldb::exec("INSERT INTO notes (body, pinned, score) VALUES (?, ?, ?)",
                            json::array({"hello", true, 1.5}));
    check(e1["ok"] == true && e1["changes"] == 1, "insert changes=1");
    check(e1["lastInsertRowid"] == 1, "insert rowid=1");

    // injection attempt is treated as DATA, not SQL (parameter binding)
    hulldb::exec("INSERT INTO notes (body, pinned, score) VALUES (?, ?, ?)",
                  json::array({"x'); DROP TABLE notes;--", false, 2.0}));
    json cnt = hulldb::get("SELECT COUNT(*) AS n FROM notes", json::array());
    check(cnt["row"]["n"] == 2, "injection string stored as data (table intact, 2 rows)");

    // query returns typed rows
    json rows = hulldb::query("SELECT id, body, pinned, score FROM notes ORDER BY id", json::array());
    check(rows["rows"].size() == 2, "query returned 2 rows");
    check(rows["rows"][0]["body"] == "hello", "text column maps to string");
    check(rows["rows"][0]["pinned"] == 1, "boolean stored as int 1");
    check(rows["rows"][0]["score"] == 1.5, "real column maps to float");

    // get with a bound param
    json one = hulldb::get("SELECT body FROM notes WHERE id = ?", json::array({2}));
    check(one["row"]["body"] == "x'); DROP TABLE notes;--", "param select returns exact stored data");

    // transaction rollback on error (second statement fails -> first reverts)
    bool threw = false;
    try {
      hulldb::batch(json::array({
        {{"sql", "INSERT INTO notes (body) VALUES (?)"}, {"params", json::array({"temp"})}},
        {{"sql", "INSERT INTO notes (id, body) VALUES (1, 'dup')"}}, // PK conflict -> error
      }));
    } catch (const std::exception&) { threw = true; }
    check(threw, "bad batch threw");
    json cnt2 = hulldb::get("SELECT COUNT(*) AS n FROM notes", json::array());
    check(cnt2["row"]["n"] == 2, "rollback reverted the partial batch (still 2 rows)");

    // null handling
    hulldb::exec("INSERT INTO notes (body, pinned, score) VALUES (?, ?, ?)",
                  json::array({"n", nullptr, nullptr}));
    json nrow = hulldb::get("SELECT pinned, score FROM notes WHERE body = ?", json::array({"n"}));
    check(nrow["row"]["pinned"].is_null() && nrow["row"]["score"].is_null(), "NULL columns map to null");
  } catch (const std::exception& e) {
    std::printf("FAIL exception: %s\n", e.what());
    failures++;
  }

  std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
              failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
