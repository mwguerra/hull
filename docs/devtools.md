# Developer tools: browser dev mode + the inspector

Two dev-only features that make iterating fast — both stripped from production.

## Browser dev mode — `hull dev --browser`

Run your UI in a **normal browser** (full Vite/Vue/React HMR) while bridge calls still
reach the real native backend. Change a label, hit reload — **no recompile**.

```bash
hull dev --browser
#   app:       http://localhost:5173/
#   inspector: http://localhost:5173/__hull/devtools
#   bridge:    http://127.0.0.1:<port>
```

How it works:

```
Browser tab (Vite, HMR)
  bridge.invoke ──HTTP POST /bridge/invoke──▶  hull-host --serve <port>  (real C++ bindings)
  bridge.on    ◀──── SSE GET /bridge/events ──  (settings:changed, … + the dev __trace)
```

- The host runs **headless** in serve mode (no window); the UI lives in your browser.
- `hull dev --browser` injects `window.__HULL_BRIDGE__` into the page; the bridge
  auto-switches to the HTTP/SSE transport. **Your app code is unchanged** — `db.*`,
  `files.*`, `httpPost`, etc. all work.
- Use `isNative()` (webview only) vs `hasBridge()` (webview **or** browser dev) to gate
  native-only logic.

> Transport is HTTP + SSE over the cpp-httplib already linked — no extra dependency.

## The inspector window

A second, dev-only window that shows everything crossing the bridge: **calls** (name,
args, result, duration, ok/error), **C++→UI events**, **database** and **file** ops,
and a **slowest-bindings** summary.

- **Browser mode:** opens as a second browser tab (`/__hull/devtools`).
- **Native dev (`hull dev`):** the app runs in the native window and the inspector
  opens as a browser tab, fed by a trace server the host runs alongside the window.

It's a standalone Vue app shipped in the package; it subscribes to the host's `__trace`
SSE channel (enabled by the host's `--inspect` flag, which `hull dev` passes
automatically). Build/refresh it with `npm run build:devtools`.

## Stripped from the deploy build

Everything above is **dev-only** and never ships:

- The HTTP/SSE transport in the bridge is gated behind `import.meta.env.DEV`, which Vite
  replaces with `false` in `hull build` — Rollup dead-code-eliminates it, so `app.html`
  contains **no** `EventSource`/`fetch`-to-bridge code (verified: the strings are absent
  from the production bundle).
- The inspector is a separate app served only by `hull dev`; it's never part of your
  app bundle.
- The host's serve/trace code is dormant in the prebuilt binary and only activates with
  the dev `--serve`/`--inspect` flags, which production launches never pass.

## Host flags (for reference)

| Flag | Meaning |
|------|---------|
| `--serve <port>` | headless HTTP/SSE bridge (browser dev mode) |
| `--inspect` | enable the dev trace (mirrors calls/events on `__trace`) |
| `--inspect-port <port>` | (window mode) also run a trace server for the inspector tab |

You don't pass these yourself — `hull dev` / `hull dev --browser` do.
