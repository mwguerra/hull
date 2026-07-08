# Hull — Deep Project Analysis

*Last reviewed: 2026-07-08 (repo state: `main`, after the fullscreen + link-policy feature landed). File references are `path:line` at the time of writing.*

This is a code-level assessment of the whole project: what Hull is, how its pieces fit,
what is genuinely strong, and where the real risks live. It is written to be re-read
before building on any subsystem — each risk names the exact file so it can be fixed
or consciously built around.

---

## 1. What Hull is (in one paragraph)

Hull turns a Vite web UI into a tiny native desktop app without Electron: a **single
generic, prebuilt C++ host binary** (~3 MB) renders any app in the OS web view
(WebView2 / WKWebView / WebKitGTK 6), parameterized entirely by CLI flags (`--url` for
dev, `--app` for a packaged single-file `app.html`, `--app-id`, `--title`, geometry,
`--fullscreen`). Native capabilities (HTTP/TLS, settings, keychain, printing, SQLite,
files, window control, links) are exposed to JS through a transport-agnostic JSON
bridge. Because the host is generic, app developers never compile C++ — `npm install`
pulls the right platform binary as an os/cpu-gated optional dependency.

## 2. Architecture

```
app UI (Vue/React/vanilla + Vite)
   │  import { db, files, httpGet, setFullscreen, … } from "@mwguerra/hull/bridge"
   ▼
bridge runtime (JS)      src/bridge/bridge-core.js — picks a transport:
   ├─ "native"  window.<binding> functions injected by the host web view
   ├─ "http"    POST /bridge/invoke + SSE /bridge/events (dev-only, DCE'd from prod)
   └─ "none"    plain browser; calls reject gracefully
   ▼
Dispatcher (C++)         host/src/dispatcher.hpp — name -> handler, emit(), __trace
   ├─ window transport   main.cpp binds every handler onto window.<name>
   └─ serve transport    serve.hpp (httplib), also reused as the inspector trace server
   ▼
bindings                 host/src/bindings/*.hpp — http, storage, credentials,
                         database, files, printer (+ window/links in main.cpp)
   ▼
cores (webview-free)     db_core, file_store, keychain, secure (crypto), paths,
                         url_policy — deliberately dependency-light and unit-testable
```

**Key design decisions and why they matter:**

- **Generic prebuilt host.** One binary runs any app. This is the project's core bet:
  it buys "npm install and go" but means every new native capability ships to *all*
  apps via the platform packages, and per-app native code requires `hull eject`
  (which then forfeits the prebuilt distribution path — documented in
  `docs/native-code.md`).
- **Transport-agnostic dispatcher.** Bindings register once (`main.cpp` `register_all`)
  and are exposed identically over the web view and over HTTP/SSE. This is what makes
  browser dev mode, the inspector, and the e2e test harness (`packages/hull/test/e2e/`)
  possible with zero per-binding wiring.
- **Single-file production app.** `hull build` produces one `app.html`
  (vite-plugin-singlefile) loaded via `file://` (or `loadFileURL` on macOS) — no dev
  server, no asset resolution at runtime, and the `import.meta.env.DEV` gate dead-code
  eliminates the HTTP transport from shipped apps.
- **Webview-free cores.** `db_core.hpp`, `file_store.hpp`, `secure.hpp`, `keychain.hpp`,
  `paths.hpp`, `url_policy.hpp` compile without the webview, which is why the standalone
  tests in `host/test/` can exist.

## 3. Subsystem review

### Host runtime (`host/src/main.cpp`)
Window mode wires the dispatcher onto the web view, injects the **link policy** script
at document start (`link_script.hpp`), and applies `--fullscreen` once the UI loop is
live. Serve mode runs the same dispatcher headless over HTTP/SSE. `--no-bridge`
creates a plain web-view window — no bindings, no emit sink, no link policy — used for
`openWindow` child windows so remote content never sees the bridge. Notable
platform work: the WebKitGTK sandbox userns probe (`main.cpp:111`), Linux desktop-file
integration for icons (`main.cpp:147`), and the macOS `loadFileURL` path
(`main.cpp:241`).

### Bridge & dispatcher
`dispatcher.hpp` is small and correct: handlers reply exactly once (sync or async),
`emit()` pushes C++→UI, and `--inspect` mirrors everything onto `__trace` for the
inspector. The JS side (`bridge-core.js`) auto-detects its transport and the public
API (`src/bridge/index.js`) wraps envelopes ergonomically (`db.*`, `files.*` throw on
error; most others return `{ok, ...}`).

### Window control & links (new)
- `url_policy.hpp` — pure allowlist: `http/https/mailto/tel` may leave the app;
  only `http/https` may open in a child window; child argv always carries
  `--no-bridge`. Unit-tested (`host/test/url_policy_test.cpp`).
- `shell.hpp` — OS default-handler open (ShellExecuteW / NSWorkspace / gio) and
  double-fork detached child spawn. `HULL_SHELL_DRYRUN=1` makes both print instead of
  act (the e2e hook).
- `window_ctl.hpp` — native fullscreen: Win32 borderless (style+placement save/restore),
  macOS `toggleFullScreen:` (animated, async), GTK4 `gtk_window_fullscreen`.
- `link_script.hpp` — injected at document start on every navigation. Two layers:
  an **origin guard** (any non-app-origin document gets every binding, the webview
  RPC object, and `__bridgeEmit` stripped before its first script runs) and the
  **link policy** (external links — HTML/SVG anchors, `<area>`, middle-click,
  `window.open` — go to the OS browser; `data-hull-window` → new Hull window;
  `data-hull-ignore`, `download`, same-origin and `#hash` links keep default
  behavior).

### Storage, database, files, credentials
- **SQLite** (`db_core.hpp`): parameterized-only, single statement per `exec`
  (stacked-statement injection impossible), `batch` = one transaction, WAL,
  `trusted_schema=OFF`, hardened compile flags (verified: `CMakeLists.txt:83`).
  SQLCipher in the secure build.
- **Files** (`file_store.hpp`): path-traversal-safe names, atomic `.tmp`+rename writes,
  bytes pass the secure layer.
- **Settings** (`bindings/storage.hpp`): one encrypted JSON blob; every write is
  read-decrypt-modify-rewrite **on the UI thread** (see risks).
- **Credentials** (`keychain.hpp`): write-only from JS by design — secrets can be
  stored/checked/erased but never read back. The HTTP binding injects them as Bearer
  tokens server-side (C++), which is the right shape.
- **Secure layer** (`secure.hpp`): self-describing at-rest format (tag byte), so a
  non-secure build fails loudly on encrypted data instead of corrupting it.

### CLI (`src/cli/`)
`dev` (native window or `--browser` with full HMR + inspector), `build` (single-file
UI + per-platform bundle/launcher/.app), `start`, `installer` (.dmg/.deb/.exe),
`doctor` (excellent preflight: missing-library → distro-package mapping in
`diagnose.js`), `eject`. Config comes from `.hullrc` deep-merged over package defaults
(`config.js`). Packaged apps **bake host flags into launchers** (`release.js`) — there
are four launch paths (dev spawn, win `.cmd`, unix launcher, macOS `.app` script), so
any new host flag must be added to all of them (the fullscreen tests in
`packages/hull/test/config.test.mjs` pin this).

### Dev tooling
Browser dev mode + the Vue inspector (`devtools/`) observing `__trace` over SSE is a
genuinely good debugging story: every call/reply/event with timings, filterable, with
a slowest-bindings summary. Dev-only by construction (loopback + DEV-gated).

## 4. Strengths worth preserving

1. **Security defaults**: parameterized single-statement SQL, write-only credentials,
   TLS always verified (`bindings/http.hpp:41`), `0600`/`0700` on-disk permissions,
   loopback-only dev servers, DEV-gated dev transports, fail-closed URL policy, and
   `--no-bridge` isolation for remote-content windows.
2. **The generic-host model** keeps app installs at ~8 MB unpacked with zero end-user
   toolchain.
3. **Honest docs**: `platforms.md` documents its own deprecated-API and UTF-16
   shortcuts; `native-code.md` admits the eject/prebuilt tension; the README's
   "Status / Not yet" section is candid.
4. **DX investment**: `doctor`, per-step `-v` timings, browser dev mode, inspector,
   Linux sandbox auto-probe with an explanatory message.

## 5. Risks & weaknesses (prioritized)

### P0 — correctness/data loss
1. **Silent settings wipe** (`bindings/storage.hpp:29-31`): `read_settings()` swallows
   decrypt/parse failures and returns `{}`; the next `saveSetting` persists that empty
   map — all other settings are gone with no error. A corrupt blob or a keychain
   hiccup in a secure build is enough. Fix: distinguish "missing" from "unreadable";
   refuse writes (or back up the blob) when unreadable.
2. **No CI test execution.** `host/test/*.cpp` and the JS tests now exist and run
   locally (`npm test`, `npm run test:e2e`), but `.github/workflows/release.yml` still
   builds and publishes **without running any of them**. One `test` job per OS before
   publish would close the gap.

### P1 — security hardening
3. **Bindings had no origin check** (`main.cpp` bind loop) — now **largely
   mitigated** by the injected origin guard (`link_script.hpp`): documents outside
   the app's origin get all bindings + the webview RPC object stripped at document
   start (e2e-verified, including via programmatic `location.href` navigation).
   Residual exposure: the guard is itself an injected user script, so it depends on
   the engine running user scripts on every new document (true on all three web
   views today); a native navigation-policy hook would be a stronger second layer
   if the webview library ever exposes one.
4. **Launcher scripts don't escape config values** (`release.js` `writeLauncher` /
   `writeMacApp`): `cfg.title` / `cfg.appId` are interpolated into `.cmd`/`.sh`
   bodies inside double quotes with no escaping — a title containing `"` or `$(...)`
   breaks or injects. (The Inno/XML paths *do* escape.) Low likelihood (developer-
   controlled values) but cheap to fix alongside a `sanitize`-style shell quoting
   helper.
5. **Unchecked OpenSSL return codes** (`secure.hpp:55-67`): `RAND_bytes`/`EVP_*`
   results ignored; also the data key is re-fetched from the keychain **per call**
   (perf) and never zeroized in memory.

### P2 — robustness/scale
6. **UI-thread blocking bindings**: credentials (`bindings/credentials.hpp`) and
   settings (`bindings/storage.hpp`) run synchronously on the calling thread, unlike
   db/files/http which use workers. A keychain prompt or slow disk stalls the UI.
7. **Unbounded threads + serialized DB**: every `db*`/`files*`/`http*` call spawns a
   detached `std::thread` (`bindings/database.hpp:15`), all DB work then serializes
   on one mutex/connection (`db_core.hpp:29`) — thread churn with no concurrency win.
   A single worker queue would be simpler and faster.
8. **`/bridge/invoke` can hang a worker forever** (`serve.hpp:83-85`): `fut.get()`
   with no timeout if a handler never replies. Dev-only, but it also backs the
   inspector trace server that runs alongside `hull dev`.
9. **No file-size limits**: `files.read`/`write` round-trip whole files through
   base64 JSON in memory (`bindings/files.hpp:30`); a large upload can exhaust memory.

### P3 — coverage/reach
10. **Platform gaps**: no `darwin-x64` (Intel mac), `linux-arm64`, or `win32-arm64`
    packages (`host.js:6`); `docs/distribution.md` still advertises a `macos-13 (x64)`
    CI job that doesn't exist in `release.yml`.
11. **HTTP binding is thin**: GET/POST only, JSON-only bodies, no custom headers, and
    the keychain token key ignores the port (`bindings/http.hpp:22-27`).
12. **Unsigned artifacts**: no code-signing/notarization anywhere (documented).
13. **Windows non-ASCII corruption** in keychain + printer names (byte-by-byte
    `std::wstring` widening, `keychain.hpp:24`) — documented, but a real bug for
    non-English users.

## 6. Testing posture

| Layer | What exists | How to run |
|-------|-------------|------------|
| C++ cores | `db_test.cpp`, `secure_files_test.cpp`, `url_policy_test.cpp` (standalone) | compile line in each header; `npm test` runs the url-policy one |
| JS CLI | `packages/hull/test/config.test.mjs` (config + all four launcher paths) | `npm test` (node --test) |
| End-to-end | `packages/hull/test/e2e/run.mjs` + fixture — real host window, link policy asserted over `__trace` SSE, fullscreen driven over HTTP invoke, `HULL_SHELL_DRYRUN` shell stub | `npm run test:e2e` (needs a built host) |
| CI | **none of the above run in CI** — release.yml only builds/publishes | — |

The e2e harness pattern (trace server as a test oracle) is worth extending to the
other bindings — it exercises the real binary, real web view, and real bridge.

## 7. Suggested roadmap (highest value first)

1. Fix the settings wipe-on-corruption path (P0.1).
2. Add a CI `test` job per OS: build host → `npm test` → `npm run test:e2e`
   (headless Linux needs `xvfb-run`), gating `release.yml` (P0.2).
3. Origin-guard the bridge bindings (P1.3).
4. Shell-escape launcher interpolations (P1.4) and check OpenSSL return codes (P1.5).
5. Replace detached-thread-per-call with one worker queue; move credentials/settings
   off the UI thread (P2.6–7).
6. `darwin-x64` package + fix the distribution.md CI-matrix description (P3.10).
7. Window-state persistence (size/position/fullscreen across launches) — natural
   follow-on to the window-control bindings.

---

*Method note: this analysis was produced by reading every file in `packages/hull`
(host sources, bindings, CLI, bridge, examples, scripts, workflows, docs) and
verifying each claim against the source at the cited lines; the risk items above were
cross-checked rather than inferred from docs.*
