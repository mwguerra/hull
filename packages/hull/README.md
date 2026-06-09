# @mwguerra/hull

> Tiny native desktop apps from your Vanilla-JS / React / Vue UI — a prebuilt C++
> web-view host you drive with npm scripts. No compiler, no Electron, no bundled
> browser engine.

Hull ships a small prebuilt native binary that renders your existing Vite app in the
operating system's web view (WebView2 / WebKit / WebKitGTK) and exposes a JSON bridge
to a C++ backend with batteries included: **TLS HTTP, encrypted storage, OS keychain,
SQLite, files, and printing**. Your app stays plain JS/React/Vue.

This README is the full reference for now.

## Contents

- [Quick start](#quick-start)
- [Integrate your project](#integrate-your-project) — **Vanilla JS · React · Vue**
- [Try it from a blank project](#try-it-from-a-blank-project)
- [Talking to the backend](#talking-to-the-backend)
- [Bridge API reference](#bridge-api-reference)
- [CLI commands](#cli-commands)
- [Configuration (`.hullrc`)](#configuration-hullrc)
- [Develop in the browser (no recompile)](#develop-in-the-browser-no-recompile)
- [Versioned releases](#versioned-releases)
- [Security (at-rest crypto is a build option)](#security-at-rest-crypto-is-a-build-option)
- [Custom native code (eject)](#custom-native-code)
- [Platform support](#platform-support)
- [How it works](#how-it-works)

## Quick start

In any existing Vite app (Vanilla JS, React, or Vue):

```bash
npm i -D @mwguerra/hull
```

Add scripts to `package.json`:

```jsonc
{
  "scripts": {
    "dev": "hull dev",      // Vite dev server in a native window (HMR)
    "build": "hull build",  // single-file UI, packaged with the host -> ./release
    "start": "hull start"   // run the packaged build
  }
}
```

`npm run dev` opens your app as a desktop window. **Zero config** — the window title
and a per-app storage namespace are derived from `package.json`, and the window ships
with the Hull logo as its icon until you set your own. Installing `@mwguerra/hull`
also pulls the prebuilt host for your OS/CPU automatically (an os/cpu-gated optional
dependency, e.g. `@mwguerra/hull-win32-x64`).

Starting a **brand-new** project? Copy one of the recipes below.

## Integrate your project

The C++ backend and the JSON bridge (`@mwguerra/hull/bridge`) are **identical across
frameworks** — only the UI layer and the optional state hook differ. Each recipe below
is the exact shape of a runnable example in the repo (`examples/vanilla-js`,
`examples/react`, `examples/vue`); every example exercises **all** features (bridge,
settings + C++→UI events, credentials, HTTP, printing, SQLite, files, single-image
upload). Copy the one you want and trim.

Every Hull project, regardless of framework, has:

- the `@mwguerra/hull` dev dependency + the npm scripts,
- a normal `vite.config.js` (Hull injects the single-file plugin only at build time),
- an `index.html` Vite entry,
- an optional [`.hullrc`](#configuration-hullrc),
- your UI code, which imports from `@mwguerra/hull/bridge`.

Project layout (same for all three):

```
my-app/
├── package.json
├── vite.config.js
├── .hullrc            # optional
├── index.html
└── src/
    ├── main.js|.jsx   # Vite entry
    ├── App.vue|.jsx   # (React/Vue) your root component
    └── style.css
```

### Vanilla JS

`package.json`:

```jsonc
{
  "name": "my-app",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "hull dev",
    "dev:browser": "hull dev --browser",
    "build": "hull build",
    "start": "hull start",
    "web": "vite"
  },
  "devDependencies": {
    "@mwguerra/hull": "^0.1.0",
    "vite": "^6.0.0"
  }
}
```

`vite.config.js`:

```js
import { defineConfig } from "vite";
// Plain Vite — no framework plugin. `hull build` adds the single-file plugin.
export default defineConfig({});
```

`index.html`:

```html
<!doctype html>
<html lang="en">
  <head><meta charset="UTF-8" /><title>My App</title></head>
  <body>
    <button id="ping">Send to C++</button>
    <pre id="out"></pre>
    <script type="module" src="/src/main.js"></script>
  </body>
</html>
```

`src/main.js` — the bridge with no framework (use `nativeSetting` for two-way state):

```js
import { ping, db, nativeSetting, hasBridge } from "@mwguerra/hull/bridge";

// 1) call C++ and show the result
document.querySelector("#ping").addEventListener("click", async () => {
  const res = await ping("hello");                 // -> { ok: true, echo: "hello" }
  document.querySelector("#out").textContent = JSON.stringify(res);
});

// 2) a two-way persisted setting (C++ stores it; C++ pushes changes back)
const theme = nativeSetting("theme");
theme.subscribe((v) => document.documentElement.classList.toggle("dark", v === "dark"));
theme.load();                                      // initial pull (no-op in a plain browser)
// theme.set("dark") persists and notifies subscribers

// 3) SQLite — works in the native host or browser dev mode
if (hasBridge()) {
  db.migrate(["CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)"])
    .then(() => db.query("SELECT * FROM notes ORDER BY id DESC"))
    .then((notes) => console.log(notes))
    .catch(console.error);
}
```

### React

`package.json`:

```jsonc
{
  "name": "my-app",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "hull dev",
    "dev:browser": "hull dev --browser",
    "build": "hull build",
    "start": "hull start",
    "web": "vite"
  },
  "dependencies": { "react": "^18.3.0", "react-dom": "^18.3.0" },
  "devDependencies": {
    "@mwguerra/hull": "^0.1.0",
    "@vitejs/plugin-react": "^4.3.0",
    "vite": "^6.0.0"
  }
}
```

`vite.config.js`:

```js
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
export default defineConfig({ plugins: [react()] });
```

`index.html`:

```html
<!doctype html>
<html lang="en">
  <head><meta charset="UTF-8" /><title>My App</title></head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.jsx"></script>
  </body>
</html>
```

`src/main.jsx`:

```jsx
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import App from "./App.jsx";
import "./style.css";

createRoot(document.getElementById("root")).render(
  <StrictMode><App /></StrictMode>
);
```

`src/App.jsx` — bridge + the `useNativeState` hook:

```jsx
import { useEffect, useState } from "react";
import { ping, db, hasBridge } from "@mwguerra/hull/bridge";
import { useNativeState } from "@mwguerra/hull/react";

export default function App() {
  const [out, setOut] = useState(null);
  const [theme, setTheme] = useNativeState("theme");   // like useState, persisted in C++
  const [notes, setNotes] = useState([]);

  useEffect(() => {
    document.documentElement.classList.toggle("dark", theme === "dark");
  }, [theme]);

  useEffect(() => {
    if (!hasBridge()) return;                           // native host or browser dev mode
    (async () => {
      await db.migrate(["CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)"]);
      setNotes(await db.query("SELECT * FROM notes ORDER BY id DESC"));
    })();
  }, []);

  return (
    <>
      <button onClick={async () => setOut(await ping("hello"))}>Send to C++</button>
      {out && <pre>{JSON.stringify(out)}</pre>}
      <select value={theme ?? ""} onChange={(e) => setTheme(e.target.value)}>
        <option value="light">Light</option>
        <option value="dark">Dark</option>
      </select>
      <ul>{notes.map((n) => <li key={n.id}>{n.body}</li>)}</ul>
    </>
  );
}
```

### Vue

`package.json`:

```jsonc
{
  "name": "my-app",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "hull dev",
    "dev:browser": "hull dev --browser",
    "build": "hull build",
    "start": "hull start",
    "web": "vite"
  },
  "dependencies": { "vue": "^3.5.0" },
  "devDependencies": {
    "@mwguerra/hull": "^0.1.0",
    "@vitejs/plugin-vue": "^5.2.0",
    "vite": "^6.0.0"
  }
}
```

`vite.config.js`:

```js
import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
export default defineConfig({ plugins: [vue()] });
```

`index.html`:

```html
<!doctype html>
<html lang="en">
  <head><meta charset="UTF-8" /><title>My App</title></head>
  <body>
    <div id="app"></div>
    <script type="module" src="/src/main.js"></script>
  </body>
</html>
```

`src/main.js`:

```js
import { createApp } from "vue";
import App from "./App.vue";
import "./style.css";

createApp(App).mount("#app");
```

`src/App.vue` — bridge + the `useNativeState` hook:

```vue
<script setup>
import { ref, watch, onMounted } from "vue";
import { ping, db, hasBridge } from "@mwguerra/hull/bridge";
import { useNativeState } from "@mwguerra/hull/vue";

const out = ref(null);
async function send() { out.value = await ping("hello"); }

const theme = useNativeState("theme");   // a ref; edits persist in C++, C++ pushes back
watch(theme, (v) => document.documentElement.classList.toggle("dark", v === "dark"),
  { immediate: true });

const notes = ref([]);
onMounted(async () => {
  if (!hasBridge()) return;              // native host or browser dev mode
  await db.migrate(["CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)"]);
  notes.value = await db.query("SELECT * FROM notes ORDER BY id DESC");
});
</script>

<template>
  <button @click="send">Send to C++</button>
  <pre v-if="out">{{ out }}</pre>
  <select v-model="theme">
    <option value="light">Light</option>
    <option value="dark">Dark</option>
  </select>
  <ul><li v-for="n in notes" :key="n.id">{{ n.body }}</li></ul>
</template>
```

### Shared `.hullrc` + run (all three)

`.hullrc` (optional — see [Configuration](#configuration-hullrc)):

```json
{
  "appId": "com.you.my-app",
  "window": { "title": "My App", "width": 1100, "height": 760 }
}
```

Then:

```bash
npm install
npm run dev           # native window with HMR (+ a dev inspector tab)
npm run dev:browser   # run the UI in your browser with the full bridge, no recompile
npm run build         # single-file the UI + package with the host -> ./release
npm run start         # run the packaged app
```

> The three recipes differ **only** in the UI layer. `@mwguerra/hull/bridge` (ping, db,
> files, settings, credentials, http, printers) is the same in all of them;
> `@mwguerra/hull/vue` and `@mwguerra/hull/react` add the `useNativeState` hook (Vanilla
> JS uses `nativeSetting` directly). For the complete, feature-by-feature versions, see
> the `examples/` apps in the repo.

## Try it from a blank project

Scaffold a fresh Vite app, add Hull, package it, and open the desktop window. The same
commands work on **Windows, macOS, and Linux** — `@mwguerra/hull` pulls the prebuilt
host for your OS/CPU automatically, and `hull start` opens the packaged app.

**Vue:**

```bash
npm create vite@latest my-hull-app -- --template vue
cd my-hull-app
npm install
npm i -D @mwguerra/hull
npx hull build      # bundle the UI + package it with the native host
npx hull start      # open the desktop app
```

**React:**

```bash
npm create vite@latest my-hull-app -- --template react
cd my-hull-app
npm install
npm i -D @mwguerra/hull
npx hull build
npx hull start
```

> Want a live-reload window without packaging first? Run `npx hull dev` instead of
> `build` + `start`. Plain JS works too — use `--template vanilla`. To wire `hull` into
> your npm scripts, see the [recipes above](#integrate-your-project).

## Talking to the backend

Every call goes UI → C++ and returns a Promise; all the real work happens in the
native host.

```js
import { ping, httpPost, saveCredential, isNative } from "@mwguerra/hull/bridge";

await ping("hello");                                  // -> { ok: true, echo: "hello" }
const res = await httpPost("https://api.example.com/x", { a: 1 }); // TLS, in C++
await saveCredential("api.example.com", "default", token);        // -> OS keychain
```

Structured persistence with embedded SQLite (parameterized, stored per-user):

```js
import { db } from "@mwguerra/hull/bridge";
await db.migrate(["CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)"]);
await db.exec("INSERT INTO notes (body) VALUES (?)", ["hello"]);
const notes = await db.query("SELECT * FROM notes ORDER BY id DESC");
```

Files / uploads (e.g. show an uploaded image — the pattern the examples use):

```js
import { files } from "@mwguerra/hull/bridge";
await files.write(file.name, file);                  // string | Uint8Array | ArrayBuffer | Blob
const bytes = await files.read(file.name);           // Uint8Array
const url = URL.createObjectURL(new Blob([bytes], { type: "image/png" }));
imgEl.src = url;                                      // preview; URL.revokeObjectURL(url) later
```

Two-way persisted state (plaintext by default; encrypted at rest in the secure build):

```js
// Vue
import { useNativeState } from "@mwguerra/hull/vue";
const theme = useNativeState("theme");   // a ref; edits persist, C++ pushes sync back

// React
import { useNativeState } from "@mwguerra/hull/react";
const [theme, setTheme] = useNativeState("theme");

// Vanilla JS
import { nativeSetting } from "@mwguerra/hull/bridge";
const theme = nativeSetting("theme");    // .get() / .set(v) / .subscribe(fn) / .load()
```

## Bridge API reference

All from `@mwguerra/hull/bridge`:

| Function | Backend |
|----------|---------|
| `ping(text)` | sync echo (diagnostics) |
| `httpPost(url, body)` / `httpGet(url)` | cpp-httplib + OpenSSL, on a worker thread; injects a `Bearer` token from the keychain |
| `saveSetting` / `loadSetting` / `loadAllSettings` | per-user store (plaintext by default; AES in the secure build) |
| `nativeSetting(key)` | two-way setting store: `.get()` / `.set(v)` / `.subscribe(fn)` / `.load()` |
| `saveCredential` / `credentialExists` / `eraseCredential` | OS keychain; **write-only** — secrets never return to JS |
| `listPrinters` | discover printers (Winspool / CUPS) |
| `printMessage(printer, text)` | print a **text document** — works with any printer (Print to PDF, OneNote, laser) |
| `printReceipt(printer, text)` / `printNetwork(host, port, text)` | raw **ESC/POS** for thermal receipt printers (spooler / TCP port-9100) |
| `db.query` / `db.get` / `db.exec` / `db.batch` / `db.migrate` | embedded SQLite, parameterized, per-user storage |
| `files.write` / `read` / `readText` / `list` / `remove` | file/upload storage in the per-user dir (through the secure layer) |
| `appInfo()` | `{ ok, appId, secure }` — `secure` true on a crypto build |
| `bridge.on(event, fn)` | subscribe to C++ → UI push events (e.g. `settings:changed`); returns an unsubscribe fn |
| `hasBridge()` / `isNative()` / `bridgeMode()` | `hasBridge` = reachable (native or browser dev); `isNative` = native web view; `bridgeMode` = `"native"`/`"http"`/`"none"` |

Framework hooks: `useNativeState(key)` from `@mwguerra/hull/vue` (returns a ref) and
`@mwguerra/hull/react` (returns `[value, setValue]`).

## CLI commands

| Command | What it does |
|---------|--------------|
| `hull dev` | Vite dev server rendered in a native window (HMR) + a dev inspector tab |
| `hull dev --browser` | run the UI in your browser with the full bridge over HTTP/SSE (no recompile) |
| `hull build [vX.Y.Z]` | single-file the UI and package it with the host into `release/<version\|development>/<platform>/` + an archive |
| `hull build … --platform <key\|all>` | also package other platforms whose host binary is present; `--format zip\|tar.gz` |
| `hull start [vX.Y.Z]` | run a packaged build |
| `hull installer [vX.Y.Z]` | wrap the build into a native installer — `.dmg` (macOS), `.deb` (Linux), `.exe` (Windows) |
| `hull eject` | copy the C++ host project into `./desktop` to add native bindings |

Add `-v` / `--verbose` to any command for per-step timings (every command prints its
total time). The version argument must match `vX.Y.Z` (optionally `-suffix`); with no
version, output goes to a `development/` folder.

> Pass **flags** like `--platform` via `npx hull …` (or the binary directly). `npm run`
> swallows unknown flags, so `npm run build -- v1.2.3` works for the version but
> `--platform` won't reach Hull through it.

## Configuration (`.hullrc`)

Drop a `.hullrc` (JSON) in your project root — only the keys you set override the
package defaults. Lookup order: `.hullrc` → `.hullrc.json` → `hull.config.json`.

```json
{
  "appId": "com.you.notes",
  "secure": false,
  "window": { "title": "Notes", "width": 1200, "height": 800, "icon": "build/icon.png" }
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `appId` | `com.hull.<pkg name>` | namespaces the store, DB, files, and keychain entries so multiple Hull apps never collide |
| `window.title` | pkg `productName`/`name` | native window title |
| `window.width` / `window.height` | `1100` / `760` | window size |
| `window.icon` (or top-level `icon`) | bundled Hull logo | PNG/ICO for the window/app icon; set at runtime on Windows (GDI+), via the app bundle on macOS/Linux; SVG is not a valid native icon |
| `secure` | `false` | run the crypto host build (`hull-host-secure`): AES files/settings + SQLCipher DB |
| `debug` | `false` | open the web-view dev tools |
| `outDir` | `dist` | Vite UI build dir |
| `releaseDir` | `release` | packaged-app output dir |

## Develop in the browser (no recompile)

```bash
npm run dev -- --browser    # or: npx hull dev --browser
```

Runs the UI in your **browser** with full Vite HMR while bridge calls still reach the
real native backend over HTTP/SSE — change a label, hit reload, no recompile. Both
`hull dev` and `--browser` also open a dev-only **inspector** (live bridge calls,
events, DB/file ops, timings) that is **stripped from production builds**
(`import.meta.env.DEV` dead-code elimination).

## Versioned releases

```bash
npm run build              # -> release/development/<platform>/ + archive
npm run build -- v1.2.3    # -> release/v1.2.3/<platform>/ + archive
```

Each build emits a self-contained, versioned bundle and a ready-to-ship archive (`.zip`
on Windows, `.tar.gz` on macOS/Linux) with the minimal runnable set — the host binary,
the libraries it needs, your inlined `app.html`, a double-click launcher, and `icon.png`
if you configured one. Unpack on the target and run. `--platform all` also packages
other platforms whose host binary is installed (realistically produced via CI, one
runner per OS). With `secure: true`, bundle dirs and archives get a `-secure` suffix.

### Native installers

After a build, wrap it into a native installer for the **current** OS:

```bash
npm run build && npx hull installer       # -> release/<version>/<App>-<version>-<key>.<dmg|deb|exe>
```

| OS | Output | Tooling |
|----|--------|---------|
| macOS | `.dmg` (the `.app` + an Applications drop-link) | `hdiutil` (built in) |
| Linux | `.deb` (installs to `/opt`, registers the `.desktop` + icon, deps via `dpkg-shlibdeps`) | `dpkg-deb` (built in) |
| Windows | `.exe` (per-user install, Start-Menu/Desktop shortcuts, uninstaller) | [Inno Setup](https://jrsoftware.org/isinfo.php) — `winget install JRSoftware.InnoSetup` |

Each is built on its own OS (the tools are OS-native), like the host. Install with:
double-click the `.dmg` and drag to Applications; `sudo apt install ./<app>.deb`; run the
`.exe`. Unsigned for now — for distribution to other machines, add code-signing
(macOS notarization / Windows Authenticode) as a later step.

## Security (at-rest crypto is a build option)

Default build = **no crypto, everything fast** (plaintext at rest; secrets still in the
keychain). For encryption at rest, use the **secure build**:

```bash
npm run build:host:secure    # AES for files/settings + SQLCipher for the DB
# then in .hullrc: { "secure": true }
```

Files and the DB go through one crypto **layer** — nothing calls cryptography directly.
SQLite is also hardened in all builds: `PRAGMA trusted_schema=OFF` on every connection,
and the default build compiles with `SQLITE_OMIT_LOAD_EXTENSION` and `SQLITE_DQS=0`.
Queries are always parameterized (bound in C++), and `exec`/`query`/`get` run one
statement each.

## Custom native code

Need your own C++ binding? Run `hull eject` to copy the host project into `./desktop`,
add a binding (`d.on("myThing", (args, reply) => reply({ ok: true }))`), and build it
with CMake. The standard bindings (HTTP / storage / keychain / printing / DB / files)
are already there to extend. See `desktop/README.md`.

## Platform support

| | Windows | macOS | Linux |
|---|---------|-------|-------|
| Web view | WebView2 (Edge) | WebKit | WebKitGTK 6 |
| Credentials | Credential Manager | Keychain | libsecret |
| Printing | Winspool | CUPS | CUPS |
| Window icon | runtime (GDI+) | `.app` bundle (built by `hull build`) | auto `.desktop` + icon-theme install |
| `hull build` output | folder + `.cmd` launcher (zip) | **`.app` bundle** (tar.gz) | folder + `.sh` launcher (tar.gz) |
| Build the host on | Windows | macOS | any OS via Docker, or native Linux |

End users only need the OS web-view runtime (preinstalled on Windows 11 and macOS;
`libwebkitgtk-6.0` on Linux). A host must be built on its own OS — true cross-compile
isn't realistic for WebView2/WebKit — except Linux, which builds from any OS via Docker.

**Linux sandbox note:** WebKitGTK sandboxes its subprocesses with bubblewrap, which
needs unprivileged user namespaces. They're blocked on Ubuntu 24.04 (AppArmor default)
and in many containers, which otherwise crashes the app with
`bwrap: setting up uid map: Permission denied`. Hull's host **auto-detects** this and
disables the sandbox so the app still runs (with a notice). Override with
`hull start --no-sandbox`, `.hullrc` `{ "linux": { "sandbox": false } }`, or keep it by
enabling userns (`sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0`). See
[platforms.md](https://github.com/mwguerra/hull/blob/main/docs/platforms.md#troubleshooting).

**Linux icon note:** GTK4 has no runtime "set icon from a PNG", so on `hull dev`/`start`
the host auto-installs desktop integration — it writes `~/.local/share/applications/<appId>.desktop`
and the icon into the user icon theme, and sets the window's app-id so the compositor
shows it (Wayland + X11). A new icon may need a moment or a re-login for the shell to
pick it up. See [configuration.md](https://github.com/mwguerra/hull/blob/main/docs/configuration.md#the-windowicon-key).

## How it works

- Prebuilt host binaries are delivered as platform-gated optional dependencies
  (`@mwguerra/hull-win32-x64`, …) — npm installs only the one for your machine.
- `hull build` uses your project's Vite plus `vite-plugin-singlefile` to inline the
  whole UI into one HTML file, then bundles it with the host.
- The host loads that file at runtime (`--app`) in production, or your dev server
  (`--url`) during development. The bridge is exposed over the web view natively, or
  over HTTP/SSE in browser dev mode.

## License

MIT
