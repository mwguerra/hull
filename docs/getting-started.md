# Getting started

## Requirements

- **Node.js** 18+
- A supported desktop OS with its web view runtime:
  - **Windows** — WebView2 runtime (ships with Windows 11; on Windows 10 install the Evergreen runtime).
  - **macOS** — WKWebView (built in).
  - **Linux** — WebKitGTK (`libwebkitgtk-6.0` or `libwebkit2gtk-4.1`).

No C++ toolchain is required — the native host is prebuilt and installed by npm.

## Add Hull to a Vite app

Starting from any Vite + Vue/React (or vanilla) project:

```bash
npm i -D @mwguerra/hull
```

Add three scripts to `package.json`:

```jsonc
{
  "scripts": {
    "dev": "hull dev",      // Vite dev server inside a native window (HMR)
    "build": "hull build",  // single-file UI, packaged with the host -> ./release
    "start": "hull start"   // run the packaged app
  }
}
```

Run it:

```bash
npm run dev              # develop (HMR in a native window)
npm run build            # -> release/development/<platform>/ + archive
npm run build -- v1.2.3  # -> release/v1.2.3/<platform>/ + archive (vX.Y.Z)
npm run start            # launch the latest development build
npm run start -- v1.2.3  # launch a specific versioned build
```

Each `build` produces a self-contained, versioned bundle plus a ready-to-ship
archive (`.zip` on Windows, `.tar.gz` on macOS/Linux) containing the minimal
runnable set. See [distribution.md](distribution.md).

## Zero-config defaults

With nothing but a `package.json`, Hull still works:

| Setting | Default |
|---------|---------|
| Window title | `package.json` `productName`, else the (unscoped) `name` |
| Window size | 1100 × 760 |
| `appId` (storage/keychain namespace) | `com.hull.<name>` |
| Build output | `dist/` (UI) → `release/` (packaged app) |

`hull build` automatically injects `vite-plugin-singlefile`; you do **not** edit
your `vite.config`.

## Optional configuration

Create `.hullrc` (JSON) next to `package.json`. Only the keys you set override the
package defaults:

```json
{
  "appId": "com.you.notes",
  "secure": false,
  "window": { "title": "Notes", "width": 1200, "height": 800 }
}
```

`appId` namespaces the settings store, database, files, and keychain entries, so two
Hull apps on the same machine never collide. `secure: true` selects the crypto host
build (encrypted at rest). Full reference: [configuration.md](configuration.md).

## Using the bridge in your UI

```js
import { ping, httpPost, hasBridge } from "@mwguerra/hull/bridge";

if (hasBridge()) {                          // native web view OR browser dev mode
  const res = await ping("hello");          // { ok: true, echo: "hello" }
  const out = await httpPost("https://api.example.com/x", { a: 1 });
}
```

`hasBridge()` is `true` in the native host and under `hull dev --browser`, and
`false` in a plain browser (`npm run web`) — so you can degrade gracefully during
pure-UI work. (`isNative()` is narrower: native web view only.) See
[features.md](features.md) for the full API and [devtools.md](devtools.md) for
browser dev mode + the inspector.

## CLI commands

| Command | What it does |
|---------|--------------|
| `hull dev` | start Vite + open the native window (HMR) + the inspector tab |
| `hull dev --browser` | run the UI in your **browser** (full HMR, no recompile) + inspector tab — see [devtools.md](devtools.md) |
| `hull build [vX.Y.Z]` | bundle the UI and assemble `release/<version\|development>/<platform>/` + an archive |
| `npx hull build … --platform <key\|all> [--format zip\|tar.gz]` | also package other platforms whose host binary is installed (use `npx`: `npm run` swallows flags) |
| `hull start [vX.Y.Z]` | run a packaged build (defaults to `development`) |
| `hull installer [vX.Y.Z]` | wrap the build into a native installer — `.dmg` (macOS), `.deb` (Linux), `.exe` (Windows) |
| `hull eject` | copy the C++ host into `./desktop` for custom native code |
| `hull help` | usage |

Add `-v` / `--verbose` to any command for per-step timings (every command also prints
its total time).
