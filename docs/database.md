# Database (SQLite)

Hull embeds **SQLite** in the C++ host so any app can store structured, queryable
data with no setup. The DB lives in the per-user app dir (namespaced by `appId`),
queries are parameterized in C++, and everything runs on a worker thread so the UI
never blocks.

```js
import { db } from "@mwguerra/hull/bridge";

// 1. Set up the schema once (ordered, run-once migrations via PRAGMA user_version)
await db.migrate([
  "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')))",
]);

// 2. Write (always pass values via the params array — never string-concatenate)
const { lastInsertRowid } = await db.exec("INSERT INTO notes (body) VALUES (?)", ["hello"]);

// 3. Read
const notes = await db.query("SELECT * FROM notes ORDER BY id DESC");
const one  = await db.get("SELECT * FROM notes WHERE id = ?", [lastInsertRowid]); // row | null

// 4. Atomic multi-statement (one transaction; rolls back on any error)
await db.batch([
  { sql: "UPDATE notes SET body = ? WHERE id = ?", params: ["edited", 1] },
  { sql: "DELETE FROM notes WHERE id = ?", params: [2] },
]);
```

All three examples ([vanilla-js](../examples/vanilla-js), [react](../examples/react),
[vue](../examples/vue)) include a working notes CRUD built on this.

## API

| Method | Returns | Use for |
|--------|---------|---------|
| `db.exec(sql, params?)` | `{ changes, lastInsertRowid }` | INSERT / UPDATE / DELETE / DDL (one statement) |
| `db.query(sql, params?)` | `row[]` | SELECT |
| `db.get(sql, params?)` | `row \| null` | SELECT first row |
| `db.batch(statements)` | `results[]` | several `{ sql, params }` atomically (transaction) |
| `db.migrate(steps)` | applied version | ordered, run-once schema setup |

The helpers unwrap the bridge envelope and **throw** on error, so use plain
`try/catch`. The underlying bindings (`dbExec`/`dbQuery`/`dbGet`/`dbBatch`) are also
available via `bridge.invoke` if you want the raw `{ ok, ... }` form.

### Parameters and types

Pass parameters positionally with `?` placeholders and a values array. Supported
JS types: `null`, `boolean` (stored 0/1), `number` (integer or float), `string`.
For objects/arrays, `JSON.stringify` into a `TEXT` column. Returned rows map
SQLite types to JSON: INTEGER→number, REAL→number, TEXT→string, NULL→null,
BLOB→base64 string.

> JS numbers are IEEE-754 doubles: integers beyond 2^53 lose precision. Store very
> large IDs as TEXT if you need exact values.

### Migrations

`db.migrate(steps)` tracks the schema version in `PRAGMA user_version`. Each array
entry is one version; on launch, only the not-yet-applied steps run (inside a
transaction), then the version is bumped. Append new steps over time — never edit or
reorder existing ones.

```js
await db.migrate([
  "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)",   // v1
  "ALTER TABLE notes ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0",     // v2
  "CREATE INDEX idx_notes_pinned ON notes(pinned)",                     // v3
]);
```

## Security & performance

What you get by default (cheap, high-value — see [security.md](security.md)):

- **Parameterized queries**, bound in C++ — the params array is data, never SQL.
  This is the main defense against injection.
- **One statement per call** for `exec`/`query`/`get` (`sqlite3_prepare_v2` ignores
  trailing SQL), so `"…; DROP TABLE x"` can't be smuggled in. Multi-statement work
  is explicit via `batch`/`migrate` arrays.
- **Per-user storage**: `<app_data_dir>/app.db`, `chmod 0600` on POSIX; Windows
  `%LOCALAPPDATA%` is per-user. Namespaced by `appId`.
- **Standard hardening** (not opt-in): `PRAGMA trusted_schema=OFF` on every connection
  in all builds; the default build also compiles SQLite with
  `SQLITE_OMIT_LOAD_EXTENSION` (no code loading via SQL), `SQLITE_DQS=0`, and foreign
  keys on. The secure build uses SQLCipher.
- **Fast**: WAL journaling, `synchronous=NORMAL`, `busy_timeout=5000`; all calls run
  on a worker thread. FTS5 full-text search is compiled in.

Where to put what:

| Data | Use |
|------|-----|
| Tokens, passwords | the **keychain** (`saveCredential`) — never the DB |
| Small sensitive values | **encrypted settings** (`saveSetting`) |
| Structured / queryable app data | **SQLite** (`db.*`) |

### Full at-rest encryption (build option)

The DB file is plaintext by default (fast; secrets belong in the keychain). For
whole-database encryption, use the **secure host build** — the DB backend switches
from vanilla SQLite to **SQLCipher** (AES, key from the keychain via `PRAGMA key`),
and files/settings switch to AES-256-GCM at the same time. It's the same `db.*` API.

```bash
npm run build:host:secure      # build the crypto host for this platform
# then in .hullrc:  { "secure": true }
```

Build deps for the secure host: SQLCipher (Linux `libsqlcipher-dev`, macOS
`brew install sqlcipher`, Windows `vcpkg install sqlcipher`). See
[security.md](security.md) and [configuration.md](configuration.md). It's opt-in
because of the per-platform dependency and per-page crypto cost.

## Custom queries in C++

Need the SQL itself to live in C++ (not the UI)? `hull eject` and add your own
bindings on top of `db_core.hpp` (the webview-free SQLite core). See
[native-code.md](native-code.md).
