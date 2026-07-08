# Features

Everything is imported from `@mwguerra/hull/bridge` (framework-agnostic), with
thin reactive adapters in `@mwguerra/hull/vue` and `@mwguerra/hull/react`.
Each call goes UI → C++ and returns a `Promise`. All three examples
([vanilla-js](../examples/vanilla-js), [react](../examples/react),
[vue](../examples/vue)) demonstrate all core features below. The package ships
TypeScript definitions for `./bridge`, `./vue`, and `./react`, so every call
below is typed and autocompleted with zero configuration.

| Function | Backend |
|----------|---------|
| `ping(text)` | sync echo (diagnostics) |
| `http.request/get/post/put/patch/delete` · `http.download` | full TLS client in C++ (headers, JSON/raw/multipart bodies, timeouts, redirects, streamed downloads); `httpGet`/`httpPost` kept for compat |
| `saveSetting` / `loadSetting` / `loadAllSettings` | per-user store (plaintext by default; AES-256-GCM in the secure build) |
| `saveCredential` / `credentialExists` / `eraseCredential` | OS keychain; **write-only** from the UI |
| `listPrinters` / `printMessage` (text doc, any printer) / `printReceipt` / `printNetwork` (ESC/POS thermal) | Winspool / CUPS; port-9100 |
| `db.query` / `db.get` / `db.exec` / `db.batch` / `db.transaction` / `db.migrate` / `db.backup` | embedded SQLite, parameterized, per-user storage — see [database.md](database.md) |
| `files.write` / `read` / `readText` / `list` / `remove` | file/upload storage in the per-user dir (through the secure layer) |
| `files.readAt` / `readTextAt` / `writeAt` | raw-byte IO at absolute paths picked via `dialogs.*` |
| `appWindow.*` (fullscreen, minimize, maximize, center, setSize, getBounds, …) | native window control — see [§9](#9--window-control--links) |
| `clipboard.readText()` / `writeText(text)` | native pasteboard; `navigator.clipboard` fallback in a browser |
| `dialogs.open/save/message` | native file pickers + message boxes (modal) |
| `openPath` / `revealPath` / `trashPath` | OS shell operations on local paths |
| `tray.set` / `tray.remove` | status item + menu (Windows/macOS; **not** Linux — replies `{ ok: false }`) |
| `openExternal(url)` | open `http/https/mailto/tel` with the **OS default browser/handler** — the default for every external link (see below) |
| `openWindow(url, {title,width,height})` | opt-in: open web content in a **new Hull window** (plain web view, no bridge bindings) |
| `notify(title, body)` | system notification (Windows toast/balloon, macOS Notification Center, Linux D-Bus); Web Notification API in a browser |
| `getTheme()` / `onThemeChanged(cb)` | OS dark/light theme via `prefers-color-scheme` |
| `updates.check/download/openInstaller` | assisted updates against a hosted `update-manifest.json` |
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
UI never blocks and certificates are **always verified** (never disabled).

```js
import { http } from "@mwguerra/hull/bridge";

// Any verb, custom headers, JSON bodies:
const res = await http.post("https://api.example.com/items", { name: "Widget", qty: 3 });
// res => { ok, status, headers, body }  (body is parsed JSON when possible, else a string)

const got = await http.get("https://api.example.com/items", { timeoutMs: 5000 });
await http.put("https://api.example.com/items/1", { qty: 4 });
await http.delete("https://api.example.com/items/1");

// The full form — everything is optional except url:
await http.request({
  url: "https://api.example.com/upload",
  method: "POST",
  headers: { "X-Custom": "1" },
  body: "raw text",          // object/array -> application/json; string -> raw (set a Content-Type)
  timeoutMs: 10000,
  auth: false,               // skip the keychain Bearer-token injection
  followRedirects: true,
});

// Multipart form (POST): plain fields and/or files (base64 content)
await http.request({
  url: "https://api.example.com/upload", method: "POST",
  form: [
    { name: "caption", value: "invoice" },
    { name: "file", fileName: "invoice.pdf", contentBase64: b64, contentType: "application/pdf" },
  ],
});
```

`httpGet(url)` / `httpPost(url, body)` still work — they're the original simple
wrappers, kept for compatibility.

**Downloads** stream straight to disk (no base64 round-trip through JS), with
progress pushed as bridge events:

```js
import { http, dialogs, bridge } from "@mwguerra/hull/bridge";

const { path } = await dialogs.save({ defaultName: "report.pdf" });
const off = bridge.on("http:download", ({ url, path, received, total }) =>
  console.log(`${received}/${total ?? "?"} bytes`));
await http.download({ url: "https://example.com/report.pdf", path }); // { ok, path, bytes }
off();
```

If a credential exists for the request's host (saved via `saveCredential(host, "default", token)`),
the host automatically adds an `Authorization: Bearer <token>` header — the token
never touches JavaScript. Pass `auth: false` to skip it for a specific request.

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

// Atomic multi-statement work — a synchronous builder that queues, then runs as ONE transaction:
await db.transaction((tx) => {
  tx.exec("UPDATE accounts SET balance = balance - ? WHERE id = ?", [50, a]);
  tx.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?", [50, b]);
});

// Consistent online snapshot (VACUUM INTO) — pairs naturally with dialogs.save():
await db.backup("/absolute/path/backup.db");   // fails if the destination exists
```

Full reference (API, types, migrations, transactions, backup, security,
performance, at-rest encryption): **[database.md](database.md)**.

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

For files **outside** the managed store — absolute paths the user picked with a
native dialog — use `files.readAt` / `readTextAt` / `writeAt`
(see [§12 · Native dialogs & shell](#12--native-dialogs--shell)). Those are raw
bytes at the given path; they do **not** pass through the secure layer.

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

### Window control (`appWindow`)

The full native window API, grouped under one export (fullscreen included):

```js
import { appWindow } from "@mwguerra/hull/bridge";

await appWindow.minimize();
await appWindow.maximize();            // maximize(false) restores
const { maximized } = await appWindow.isMaximized();
await appWindow.hide();                // and appWindow.show()
await appWindow.center();
await appWindow.setAlwaysOnTop(true);  // setAlwaysOnTop(false) to release
await appWindow.setSize(900, 600);
await appWindow.setPosition(100, 80);
const b = await appWindow.getBounds(); // { ok, x?, y?, width, height, maximized, fullscreen }
```

These drive the real window handle (HWND / NSWindow / GtkWindow) and only work
in the native host — in browser dev mode they reply `{ ok: false }` with an
explanation. **Linux/Wayland caveat:** the compositor owns window placement, so
`center()`, `setAlwaysOnTop()` and `setPosition()` are unsupported there (they
reply `{ ok: false }` and say so), and `getBounds()` omits `x`/`y`. Sizing,
minimize/maximize, show/hide and fullscreen work everywhere.

To persist size/position/maximized/fullscreen across launches, set
`window.rememberState` in `.hullrc` — see
[configuration.md](configuration.md#the-windowrememberstate-key).

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

## 10 · System notifications

```js
import { notify } from "@mwguerra/hull/bridge";

await notify("Order ready", "Table 12 — 2 items");   // { ok: true }
await notify("Ping");                                 // body is optional
```

An empty title falls back to the app title (native host; in a browser the title
is used as-is). Per platform:

- **Windows** — a notification balloon attached to the app window's tray icon:
  shows as a toast and lands in the **Action Center** on Windows 10/11. Uses the
  app's window icon.
- **macOS** — packaged apps (`hull start`, installed `.app`) post to the
  **Notification Center**, attributed to the app (the OS asks the user for
  permission on first use). The raw dev binary (`hull dev`) has no bundle id, so
  the host falls back to `osascript` — the notification still shows, attributed
  to Script Editor.
- **Linux** — `org.freedesktop.Notifications` over **D-Bus** (works on GNOME,
  KDE, and anything running a notification daemon). The `.hullrc` `window.icon`
  is passed through and shown when the daemon supports it.
- **Browser** (dev mode / plain web) — the **Web Notification API**; the browser
  prompts for permission, so call `notify` from a click handler the first time.

Notifications are fire-and-forget: `{ ok }` means "dispatched", not "seen" —
the OS user settings decide whether it's displayed.

**Clicks** come back as a bridge event on all three platforms (Windows balloon
click, macOS notification activation, Linux D-Bus default action):

```js
import { bridge, appWindow } from "@mwguerra/hull/bridge";
bridge.on("notification:clicked", () => appWindow.show());
```

## 11 · Clipboard

Plain-text clipboard, done in the host (`navigator.clipboard` is unreliable
inside embedded web views — permission prompts never show):

```js
import { clipboard } from "@mwguerra/hull/bridge";

await clipboard.writeText("hello");
const { text } = await clipboard.readText();   // { ok, text } — text is null when empty/no text
```

Native pasteboard per platform (Win32 clipboard / NSPasteboard / GdkClipboard —
the Linux read is GDK's async API made synchronous). In a plain browser it falls
back to `navigator.clipboard`, where the browser's permission rules apply.

## 12 · Native dialogs & shell

Native file pickers and message boxes — all **modal**:

```js
import { dialogs } from "@mwguerra/hull/bridge";

// Open file(s) or folder(s)
const o = await dialogs.open({
  title: "Pick an image",
  multiple: false,                // true -> paths[] can hold several
  directory: false,               // true -> folder picker
  filters: [{ name: "Images", extensions: ["png", "jpg"] }],   // no dots
});
// -> { ok, canceled, paths: string[] }

// Save
const s = await dialogs.save({ defaultName: "notes.txt", filters: [{ name: "Text", extensions: ["txt"] }] });
// -> { ok, canceled, path: string|null }

// Message box
const m = await dialogs.message({
  message: "Delete this note?", detail: "This cannot be undone.",
  buttons: ["Delete", "Cancel"], type: "warning", defaultId: 1,
});
// -> { ok, button: index }
```

**Windows caveat for `dialogs.message`:** it uses `MessageBoxW`, which has fixed
button sets — your button *count* maps to OK / OK+Cancel / Yes+No+Cancel and the
labels are the system's, not yours. Design cross-platform message boxes around
the count and returned index, not the label text.

The picked paths are **absolute** — read/write them with `files.readAt` /
`readTextAt` / `writeAt` (raw bytes, not the managed store), and hand them to
the shell operations:

```js
import { files, openPath, revealPath, trashPath } from "@mwguerra/hull/bridge";

const text = await files.readTextAt(o.paths[0]);
await files.writeAt(s.path, "hello");

await openPath(s.path);     // open with the OS default application
await revealPath(s.path);   // show in the file manager, selected (Linux: FileManager1 D-Bus, with fallback)
await trashPath(s.path);    // move to trash / recycle bin (reversible)
```

## 13 · Tray / status item

**Windows + macOS.** Linux is **not supported** — a modern Linux tray means
implementing the D-Bus StatusNotifierItem + dbusmenu specs (or depending on
libappindicator), neither of which fits Hull's zero-extra-deps host; `tray.set`
replies `{ ok: false }` with that explanation.

```js
import { tray, bridge } from "@mwguerra/hull/bridge";

await tray.set({
  tooltip: "My App",
  menu: [
    { id: "open",  label: "Open" },
    { id: "sep1",  label: "", type: "separator" },
    { id: "mute",  label: "Mute", type: "checkbox", checked: false },
    { id: "quit",  label: "Quit", enabled: true },
  ],
});

bridge.on("tray:menu", ({ id }) => { /* a menu item was chosen */ });
bridge.on("tray:click", () => { /* icon left-click — Windows only */ });

await tray.remove();
```

On macOS clicking the icon opens the menu (there is no separate click event);
on Windows a left-click emits `tray:click` and a right-click opens the menu.

## 14 · Theme (dark / light)

```js
import { getTheme, onThemeChanged } from "@mwguerra/hull/bridge";

getTheme();                                        // "dark" | "light" (synchronous)
const off = onThemeChanged((t) => applyTheme(t));  // fires on every OS theme change
// off();  // unsubscribe
```

Implemented via `prefers-color-scheme` — the embedded web views follow the OS
theme, so this works in the host *and* in a browser. On Linux it follows the
GTK theme.

## 15 · Single instance

Opt-in via `.hullrc`:

```jsonc
{ "singleInstance": true }
```

Launching a second copy focuses the already-running window and exits; the first
instance is told about it:

```js
import { bridge } from "@mwguerra/hull/bridge";
bridge.on("app:second-instance", () => { /* e.g. navigate home */ });
```

Under the hood: the primary instance holds an exclusive OS lock plus a loopback
port file in the app data dir; the second launch fails the lock, pings the
primary, and exits. The lock is an OS handle, so it dies with the process — no
stale-lock cleanup needed. **macOS note:** packaged `.app`s are already
single-instanced by LaunchServices; this flag covers the raw binary / `hull dev`
on macOS, and everything on Windows/Linux. See
[configuration.md](configuration.md#the-singleinstance-key).

<a id="assisted-updates"></a>
## 16 · Assisted updates

Hull does **not** silently self-patch binaries. The flow is: check a manifest →
download the new installer/archive → hand it to the user/OS.

`hull build` writes `release/<version>/update-manifest.json` next to the
archives:

```json
{
  "version": "v1.2.3",
  "generatedAt": "2026-07-08T12:00:00.000Z",
  "platforms": { "win32-x64": { "file": "App-v1.2.3-win32-x64.zip", "bytes": 3500000 } }
}
```

Host that JSON (plus the artifacts) anywhere, then in the app:

```js
import { updates, dialogs } from "@mwguerra/hull/bridge";

const res = await updates.check("https://example.com/releases/update-manifest.json", "v1.2.0");
if (res.ok && res.updateAvailable) {
  const { path } = await dialogs.save({ defaultName: res.latest.platforms["win32-x64"].file });
  await updates.download("https://example.com/releases/" + res.latest.platforms["win32-x64"].file, path);
  await updates.openInstaller(path);   // opens the installer/archive with the OS
}
```

`updates.download` streams to disk and reports progress via the same
`http:download` bridge event as `http.download` (see [§4](#4--http-tls-from-c)).
Version comparison is simple semver-ish (`v` prefix and `-suffix` tolerated).

## Browser modes

- **`hull dev --browser`** — full bridge in your browser (the host runs headless and
  the bridge talks over HTTP/SSE). `db.*`, `files.*`, `httpPost`, etc. all work; edit
  the UI and just reload. See [devtools.md](devtools.md).
- **`npm run web`** (plain `vite`) — pure-UI work with no host; bridge calls reject.

Gate native-dependent logic with **`hasBridge()`** (true in the native web view *or*
browser dev mode) rather than `isNative()` (native web view only), so the same code
runs under `hull dev --browser`.
