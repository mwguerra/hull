# Features

Everything is imported from `@mwguerra/hull/bridge` (framework-agnostic), with
thin reactive adapters in `@mwguerra/hull/vue` and `@mwguerra/hull/react`.
Each call goes UI → C++ and returns a `Promise`. All three examples
([vanilla-js](../examples/vanilla-js), [react](../examples/react),
[vue](../examples/vue)) demonstrate every feature below.

| Function | Backend |
|----------|---------|
| `ping(text)` | sync echo (diagnostics) |
| `httpPost(url, body)` / `httpGet(url)` | cpp-httplib + OpenSSL on a worker thread; injects a keychain `Bearer` token |
| `saveSetting` / `loadSetting` / `loadAllSettings` | per-user store (plaintext by default; AES-256-GCM in the secure build) |
| `saveCredential` / `credentialExists` / `eraseCredential` | OS keychain; **write-only** from the UI |
| `listPrinters` / `printMessage` (text doc, any printer) / `printReceipt` / `printNetwork` (ESC/POS thermal) | Winspool / CUPS; port-9100 |
| `db.query` / `db.get` / `db.exec` / `db.batch` / `db.migrate` | embedded SQLite, parameterized, per-user storage — see [database.md](database.md) |
| `files.write` / `read` / `readText` / `list` / `remove` | file/upload storage in the per-user dir (through the secure layer) |
| `setFullscreen(on)` / `isFullscreen()` / `toggleFullscreen()` | native window fullscreen (Win32 / macOS Spaces / GTK4); DOM Fullscreen fallback in a browser |
| `openExternal(url)` | open `http/https/mailto/tel` with the **OS default browser/handler** — the default for every external link (see below) |
| `openWindow(url, {title,width,height})` | opt-in: open web content in a **new Hull window** (plain web view, no bridge bindings) |
| `appInfo()` | `{ ok, appId, secure }` — `secure` is true on a crypto build |
| `isNative()` / `hasBridge()` / `bridgeMode()` | `isNative` = native web view; `hasBridge` = reachable (native **or** browser dev); `bridgeMode` = `"native"`/`"http"`/`"none"` |
| `bridge.on(event, cb)` | subscribe to C++ → UI push events |
| `nativeSetting(key)` / `useNativeState(key)` | two-way bound, persisted state |

---

## 1 · Bridge call (sync)

```js
import { ping } from "@mwguerra/hull/bridge";
const res = await ping("hello");   // { ok: true, echo: "hello" }
```

The simplest binding shape: C++ parses the JSON args and returns a JSON result.
Use this pattern (a synchronous binding) only for instant work.

## 2 · Settings — two-way, persisted

Values are written to a per-user file (plaintext by default; **AES-256-GCM** in the
secure build — see [security.md](security.md)). After every successful write, C++
pushes a `settings:changed` event so all subscribers stay in sync.

Direct calls:

```js
import { saveSetting, loadSetting, loadAllSettings } from "@mwguerra/hull/bridge";
await saveSetting("theme", "dark");        // { ok: true }
const { value } = await loadSetting("theme");
const all = await loadAllSettings();        // { ok: true, value: { theme: "dark", ... } }
```

Two-way reactive binding:

```js
// Vue — returns a ref; works with v-model
import { useNativeState } from "@mwguerra/hull/vue";
const theme = useNativeState("theme");

// React — mirrors useState
import { useNativeState } from "@mwguerra/hull/react";
const [theme, setTheme] = useNativeState("theme");

// Vanilla — use the Layer-2 store directly
import { nativeSetting } from "@mwguerra/hull/bridge";
const theme = nativeSetting("theme");
await theme.load();
theme.subscribe((v) => document.documentElement.classList.toggle("dark", v === "dark"));
theme.set("dark");
```

Listen to pushes yourself:

```js
import { bridge } from "@mwguerra/hull/bridge";
const off = bridge.on("settings:changed", ({ key, value }) => console.log(key, value));
// off();  // unsubscribe
```

> Settings are for **non-secret** app state. Secrets
> go in the keychain (below) and are never emitted.

## 3 · Credentials — write-only

Secrets are a C++-only concern. The UI may *collect* a credential once, but never
*receives* one back. There is intentionally no "read secret" binding.

```js
import { saveCredential, credentialExists, eraseCredential } from "@mwguerra/hull/bridge";

await saveCredential("api.example.com", "default", token); // -> OS keychain
const { exists } = await credentialExists("api.example.com", "default"); // boolean only
await eraseCredential("api.example.com", "default");
```

Stored in Windows Credential Manager / macOS Keychain / Linux Secret Service
(libsecret). See [security.md](security.md).

## 4 · HTTP (TLS, from C++)

All networking happens in C++ on a worker thread (cpp-httplib + OpenSSL), so the
UI never blocks and certificates are always verified.

```js
import { httpPost, httpGet } from "@mwguerra/hull/bridge";

const res = await httpPost("https://api.example.com/items", { name: "Widget", qty: 3 });
// res => { ok, status, body }   (body is parsed JSON when possible, else a string)

const got = await httpGet("https://api.example.com/items");
```

If a credential exists for the request's host (saved via `saveCredential(host, "default", token)`),
the host automatically adds an `Authorization: Bearer <token>` header — the token
never touches JavaScript.

## 5 · Printing

```js
import { listPrinters, printMessage, printReceipt, printNetwork } from "@mwguerra/hull/bridge";

const { printers } = await listPrinters();           // [{ name, isDefault }]
await printMessage(printers[0].name, "Hello!");       // text document — ANY printer
await printReceipt(printers[0].name, "Hello!");       // raw ESC/POS via the spooler (thermal)
await printNetwork("192.168.0.50", 9100, "Hello!");  // raw ESC/POS over TCP (thermal)
```

Discovery uses Winspool on Windows and CUPS on macOS/Linux.

- **`printMessage`** renders the text as a normal **document** (GDI on Windows, CUPS
  `text/plain` on macOS/Linux), so it works with **any** printer — Microsoft Print to
  PDF, OneNote, and physical laser printers. Single page, word-wrapped.
- **`printReceipt`** / **`printNetwork`** send a minimal **ESC/POS** receipt
  (init → text → feed → cut) to a thermal/receipt printer — via the local spooler or a
  TCP socket (port 9100). These produce garbage on document printers (use `printMessage`
  there). See [platforms.md](platforms.md).

## 6 · Database (SQLite)

Structured, queryable persistence — parameterized in C++, stored per-user.

```js
import { db } from "@mwguerra/hull/bridge";
await db.migrate(["CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)"]);
await db.exec("INSERT INTO notes (body) VALUES (?)", ["hello"]);
const notes = await db.query("SELECT * FROM notes ORDER BY id DESC");
```

Full reference (API, types, migrations, security, performance, at-rest encryption):
**[database.md](database.md)**.

## 7 · Files (uploads / blobs)

Store files in the per-user app dir (through the secure layer — plaintext by default,
AES in the secure build). Names are sanitized (no path traversal).

```js
import { files } from "@mwguerra/hull/bridge";

// e.g. from an <input type="file">
const file = inputEl.files[0];
await files.write(file.name, file);            // string | Uint8Array | ArrayBuffer | Blob

const list = await files.list();                // [{ name, size }]
const bytes = await files.read("photo.png");    // Uint8Array
const text  = await files.readText("notes.md"); // string
await files.remove("photo.png");
```

**Previewing an uploaded image.** Read the bytes back and wrap them in a `Blob`
to get a displayable object URL (revoke it when you replace/remove the image):

```js
const bytes = await files.read("photo.png");
const url = URL.createObjectURL(new Blob([bytes], { type: "image/png" }));
imgEl.src = url;                                 // <img>; URL.revokeObjectURL(url) later
```

Every example (and the consumer test app) ships a **single-image upload** section
built on exactly this: uploading a new image deletes the previous one, the chosen
file name is tracked in a setting so the preview survives restarts, and a delete
button clears it.

## 8 · Build info

```js
import { appInfo } from "@mwguerra/hull/bridge";
const info = await appInfo();   // { ok, appId, secure }
const secure = info?.secure;    // true on a crypto host build
```

## 9 · Window control & links

### Fullscreen

```js
import { setFullscreen, isFullscreen, toggleFullscreen } from "@mwguerra/hull/bridge";

await setFullscreen(true);      // { ok: true, fullscreen: true }
await setFullscreen(false);
await toggleFullscreen();
const { fullscreen } = await isFullscreen();
```

Native per platform: borderless fullscreen on Windows, the fullscreen Space on
macOS (animated — `isFullscreen()` reflects the state once the transition runs),
`gtk_window_fullscreen` on Linux. In a plain browser / `dev --browser` the calls
fall back to the DOM Fullscreen API (which requires a user gesture).

To **start** the app in fullscreen, set it in `.hullrc` (see
[configuration.md](configuration.md)) or pass `--fullscreen` to `hull dev` / `hull start`:

```jsonc
{ "window": { "fullscreen": true } }
```

### Links — external by default, in-app by opt-in

A Hull app is an app, not a browser tab. The host injects a link policy into the
web view, so **no app code is needed**:

| The user clicks / the app calls | What happens |
|--------------------------------|--------------|
| any external `http(s)` link (any `target`, left **or middle** click, HTML/SVG `<a>` or `<area>`) | opens in the **OS default browser** — the app never navigates away |
| a `mailto:` / `tel:` link | opens the OS mail client / dialer |
| `window.open("https://…")` (external) | OS default browser; returns `null` (no popup to script) |
| `window.open(anything else)` | suppressed (returns `null`) — call `openWindow()` for an in-app window |
| `<a data-hull-window href="https://…">` | **opt-in**: opens a new **Hull window** — a plain web view with **no bridge bindings** |
| `<a data-hull-ignore href="…">` | Hull leaves the link alone (web view default behavior) |
| same-origin links, `#hash` links, `download` links | untouched — normal SPA routing keeps working |

Programmatic equivalents:

```js
import { openExternal, openWindow } from "@mwguerra/hull/bridge";

await openExternal("https://example.com/docs");                 // OS default browser
await openWindow("https://example.com/help", { title: "Help" }); // new Hull window
```

Both fail closed: `openExternal` accepts only `http/https/mailto/tel`, and
`openWindow` only `http/https` (never `file:`, `javascript:`, custom schemes).
Child windows are separate processes launched with `--no-bridge`, so remote
content **never** sees the native bindings (database, keychain, files, …). In a
browser both degrade to `window.open(url, "_blank")` — a new tab *is* the user's
default browser there.

**The origin guard.** Click interception is a UX default, not a boundary — a page
can still navigate itself (`location.href`, form submits, meta refresh). So the
host also injects an origin guard that runs at document start on **every**
navigation: any document that is not the app's own origin (`file:` for packaged
apps, the dev-server origin under `hull dev`) gets **every native binding, the
webview RPC object, and the event hook stripped** before its first script runs.
A foreign page inside the bridged window is just a page. This is covered by the
e2e suite (`npm run test:e2e`), which navigates the app window to a foreign
origin and verifies nothing leaks.

## Browser modes

- **`hull dev --browser`** — full bridge in your browser (the host runs headless and
  the bridge talks over HTTP/SSE). `db.*`, `files.*`, `httpPost`, etc. all work; edit
  the UI and just reload. See [devtools.md](devtools.md).
- **`npm run web`** (plain `vite`) — pure-UI work with no host; bridge calls reject.

Gate native-dependent logic with **`hasBridge()`** (true in the native web view *or*
browser dev mode) rather than `isNative()` (native web view only), so the same code
runs under `hull dev --browser`.
