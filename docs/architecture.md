# Architecture

Hull is a thin, fast alternative to Electron. Instead of bundling a browser
engine, it renders your web UI in the **operating system's own web view** and
puts all real work in a small **C++ backend**. The two halves talk over a
**JSON bridge**.

```
┌──────────────────────────────┐      window.<binding>(args)        ┌──────────────────────────┐
│  Your UI (Vue / React / JS)  │  ───────JSON array of args───────► │  C++ host (native)       │
│  presentation only           │                                    │  - business logic        │
│  compiled to ONE html file   │  ◄──────JSON result (Promise)───── │  - HTTP / TLS            │
│  (prod) or served by Vite     │                                    │  - storage / keychain    │
│                              │  ◄── eval() pushes events (C++→UI)  │  - printing              │
└──────────────────────────────┘                                    └──────────────────────────┘
```

## The prebuilt generic host

The defining design choice: **one prebuilt host binary runs any app.** It is not
recompiled per project. The host is parameterized at launch:

| Flag | Purpose |
|------|---------|
| `--url <url>` | load a dev server (used by `hull dev`, enables HMR) |
| `--app <file.html>` | load a built single-file bundle (used by `hull start` / the launcher) |
| `--serve <port>` | headless HTTP/SSE bridge (browser dev mode, no window) |
| `--inspect` / `--inspect-port <port>` | dev trace for the inspector |
| `--title`, `--width`, `--height` | window chrome |
| `--icon <path>` | window/app icon (Windows runtime; macOS/Linux via bundle/.desktop) |
| `--app-id <id>` | namespaces per-app storage + keychain |
| `--debug` | open dev tools |

On Linux the host also reads two env vars for the WebKitGTK sandbox —
`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` and `HULL_FORCE_SANDBOX` — which the CLI
sets from `.hullrc` `linux.sandbox` / `hull dev|start --no-sandbox` (it otherwise
auto-detects). See [configuration.md](configuration.md#the-linuxsandbox-key).

Because the host never needs to be compiled by app developers, "npm install and
go" is possible. Per-platform binaries are delivered as os/cpu-gated optional
dependencies (`@mwguerra/hull-win32-x64`, …) — npm installs only the one matching
the machine. This is the same model esbuild and swc use.

The standard bindings (HTTP, storage, keychain, printing, SQLite) are compiled into
the host and available to every app. For **app-specific** native code, `hull eject`
emits the C++ project so you can add bindings and build your own host — see
[native-code.md](native-code.md).

## The JSON bridge

Bindings register **handlers** on a transport-agnostic **dispatcher** (`dispatcher.hpp`):
a handler takes the parsed JSON args and calls `reply(resultJson)` exactly once —
synchronously for instant work (`ping`) or later from a worker thread for I/O
(`httpPost`, `db.*`, `files.*`) so the UI never blocks. C++ → UI pushes go through
`emit(event, payload)` and the UI subscribes with `bridge.on(event, handler)`.

The dispatcher is exposed over **two transports**, chosen at launch:

- **Native** — the host binds each handler onto `window.<binding>` in the web view;
  `emit` runs JS via `eval()`. This is `hull dev` / packaged apps.
- **HTTP/SSE** — the host runs headless (`--serve`) and exposes the same handlers over
  `POST /bridge/invoke` + an SSE event stream (cpp-httplib, no extra dependency). This
  is **browser dev mode** (`hull dev --browser`); the browser's `bridge-core` switches
  to `fetch` + `EventSource`. The HTTP transport is gated behind `import.meta.env.DEV`,
  so it's stripped from production builds. See [devtools.md](devtools.md).

A dev **trace** (`--inspect`) mirrors every call/reply/event on a `__trace` event that
feeds the inspector window.

### Client layering

The JS runtime is split so the framework is swappable:

```
Layer 3  Framework adapter   useNativeState (Vue ref / React hook)   ← @mwguerra/hull/vue|react
Layer 2  Domain store        nativeSetting(key): get/set/subscribe   ← reusable as-is anywhere
Layer 1  Transport core      bridge.invoke(...) + bridge.on(event)   ← @mwguerra/hull/bridge
```

One-shot calls (HTTP, printing) use `bridge.invoke` directly; only state that must
stay in sync both ways uses the store + adapter.

## The build pipeline

```
your source ──vite build + vite-plugin-singlefile──▶ dist/index.html (everything inlined)
                                                          │  hull build
                                                          ▼
                          release/  =  hull-host.exe + OpenSSL DLLs + app.html + launcher.cmd
```

- `hull dev` drives Vite's dev server via its JS API and points the host at it
  with `--url` (full HMR inside the native window), plus a companion inspector tab.
- `hull dev --browser` runs the host headless (`--serve`) and opens the UI in your
  browser — full HMR, no recompile. See [devtools.md](devtools.md).
- `hull build` runs `vite build` with `vite-plugin-singlefile` appended to your
  existing config (your `vite.config` is untouched), producing one self-contained
  HTML file, then assembles a runnable `release/` folder.
- `hull start` launches the host against `release/app.html`.

## Why this is small and fast

- No bundled Chromium: the host is ~0.5 MB; idle RAM is ~20–25 MB (vs 150 MB+).
- The UI is one inlined HTML document — no file server, no CORS at runtime.
- All heavy lifting is native C++ on worker threads; the UI thread only paints.
