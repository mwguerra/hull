# Security

Hull's backend is built around a few hard rules. They hold for the prebuilt host
and should hold for any host you build after `eject`.

## Secrets are a C++-only concern

- Secrets (API keys, tokens, passwords) live in the **OS keychain** — Windows
  Credential Manager, macOS Keychain, Linux Secret Service (libsecret).
- The UI may **write** a secret (`saveCredential`) but can never **read** one back.
  There is no "get secret" binding. `credentialExists` returns only a boolean.
- Secrets are used **inside C++** only — e.g. the HTTP bindings inject an
  `Authorization: Bearer <token>` header pulled from the keychain, keyed by the
  request host. The token never enters JavaScript.
- Never hardcode secrets in your UI source or `index.html` — the bundle is embedded
  in the shipped app and is trivially extractable.
- Secrets are never logged or returned in error messages.

```js
// OK — collect once, store in the keychain:
await saveCredential("api.example.com", "default", token);
// Then just call the API; C++ attaches the token:
await httpPost("https://api.example.com/x", { a: 1 });
```

## Transport security

- TLS certificate verification is **always on** for HTTP calls and is never
  disabled. (cpp-httplib + OpenSSL.)
- On some Linux setups you may need to point the host at a CA bundle
  (`/etc/ssl/certs/ca-certificates.crt`) — an `eject`-level change.

## Navigation & links — the bridge stays home

The native bindings are only meant for **your app's own code**, so the host
enforces two things at document start of every navigation (injected before any
page script runs):

- **Origin guard** — any document that is not the app's own origin (`file:` for
  packaged apps; the dev-server origin under `hull dev`) gets every binding, the
  webview RPC object, and the event hook **stripped**. Even if a page navigates
  itself to a remote site (`location.href`, form submit, redirect), that site
  sees a plain web page — no database, keychain, files, or HTTP bindings.
- **Link policy** — external links open in the OS default browser instead of
  navigating the app window (see
  [features.md](features.md#9--window-control--links)); `data-hull-window`
  child windows are separate `--no-bridge` processes with **no bindings at all**;
  `openExternal`/`openWindow` accept only `http/https(/mailto/tel)` and fail
  closed on `file:`, `javascript:`, `data:` and custom schemes.

Both behaviors are exercised by the e2e suite (`npm run test:e2e`), including a
programmatic navigation to a foreign origin that asserts nothing leaks.

## Encryption at rest — a build-time layer

At-rest crypto is a **layer** (`secure::`), chosen at build time. Nothing in the
codebase calls crypto directly — files, settings, and the DB all go through it.

| | Default build (fast) | Secure build (`-DHULL_CRYPTO=ON`) |
|---|---|---|
| Files (`files.*`) | plaintext | AES-256-GCM |
| Settings (`saveSetting`) | plaintext | AES-256-GCM |
| Database (`db.*`) | vanilla SQLite | **SQLCipher** (whole-DB AES) |
| Key | — | per-install 32-byte key in the **OS keychain** |
| Cost | zero | per-page/blob AES |

- **Default = no crypto, so everything is fast.** The fast host doesn't touch the
  keychain for at-rest data at all.
- Turn it on by running the **secure host build** (`npm run build:host:secure`, or
  ship `hull-host-secure`) and setting `"secure": true` in `.hullrc`
  ([configuration.md](configuration.md)). Files/settings then use AES-256-GCM and the
  DB uses SQLCipher; the key lives in the keychain, never on disk.
- The file/settings blob format is **self-describing** (a leading `0x00` plaintext /
  `0x01` AES tag): a secure build can still read old plaintext data, and a default
  build fails loudly if handed encrypted data.
- Choose the mode at project start — switching modes later means migrating existing
  data (plaintext ⇄ encrypted are not interchangeable in place).

## Files

- Stored under `<app_data_dir>/files`, named by a **sanitized basename** — path
  traversal (`../`, separators, `..`) is rejected.
- Contents pass through the same `secure::` layer (plaintext by default, AES in the
  secure build). Files are `chmod 0600` on POSIX; writes are atomic.

### Path-based IO (`files.readAt` / `writeAt`, dialogs, shell ops)

The dialog + shell features (`dialogs.open/save`, `files.readAt`/`readTextAt`/
`writeAt`, `openPath`/`revealPath`/`trashPath`) operate on **absolute paths** the
user picked — raw bytes, **not** the managed store above and **not** through the
`secure::` layer. This IO is entirely **app-code-driven**: nothing in the host
initiates it, and there is no path-traversal concern because the app is
explicitly handed arbitrary user-chosen paths. What keeps this away from remote
content is the **origin guard** (above): a foreign page never sees these
bindings, so it cannot open dialogs or read/write local paths.

## Storage location & permissions

- Data is written to the **per-user app directory**, namespaced by `appId`:
  - Windows `%LOCALAPPDATA%\<appId>` · macOS `~/Library/Application Support/<appId>`
    · Linux `$XDG_DATA_HOME/<appId>` or `~/.local/share/<appId>`.
- POSIX files are `chmod 0600`, directories `0700`. Windows relies on the per-user
  `%LOCALAPPDATA%`.
- Writes are atomic (temp file + rename).
- Never write app data next to the executable, into temp, or into a world-readable
  location.

## SQLite

- Queries are **parameterized** — the params array is bound in C++, never
  concatenated into SQL. This is the main defense against injection.
- `exec`/`query`/`get` run **one statement each** (trailing SQL is ignored), so a
  stray `"…; DROP TABLE x"` can't be stacked. Multi-statement work is explicit via
  `batch`/`migrate`.
- The DB lives in the per-user app dir (`chmod 0600` on POSIX), namespaced by `appId`.
- Standard hardening (not opt-in): `PRAGMA trusted_schema=OFF` on every connection in
  all builds; the default build additionally compiles SQLite with
  `SQLITE_OMIT_LOAD_EXTENSION` (no code loading via SQL), `SQLITE_DQS=0`, and
  `SQLITE_DEFAULT_FOREIGN_KEYS=1` (foreign keys enforced). The secure build uses
  SQLCipher, which has its own hardening.
- The DB file is **plaintext by default** (fast) — keep secrets in the keychain. For
  whole-DB encryption, use the secure build (SQLCipher); see the layer table above and
  [database.md](database.md).

## C++ → UI push events

- The `emit()` / `settings:changed` channel is for **non-secret** application state
  only. There is no subscribe/emit path that returns a secret to JavaScript.

## If you eject

- Keep TLS verification on.
- Parameterize SQL (`sqlite3_bind_*`); never concatenate SQL strings.
- Don't return secrets to the UI from any new binding.
- On macOS, prefer the modern `SecItem*` keychain APIs (see
  [platforms.md](platforms.md)).

## Checklist

- ✅ Secrets in the keychain; UI write-only.
- ✅ TLS verification on; OpenSSL required at build time.
- ✅ Per-user app dir; `0600` files; atomic writes; parameterized SQL.
- ✅ At-rest encryption available as an opt-in build (AES files/settings + SQLCipher DB),
  key in the keychain — off by default for speed.
- ❌ No hardcoded credentials, no secrets in the bundle/logs/UI, no disabled cert checks.
