// Public bridge API for app code:  import { ping, httpPost, ... } from "@mwguerra/hull/bridge";
//
// Every call goes UI -> C++ and returns a Promise. All real work (TLS HTTP,
// encrypted storage, keychain, printing) happens in the native host.

import { bridge } from "./bridge-core.js";
import { compareVersions } from "./version.js";

export { bridge } from "./bridge-core.js";
export { nativeSetting } from "./native-store.js";
export { compareVersions } from "./version.js";

const call = (name, ...args) => bridge.invoke(name, ...args);

// "native" (host web view) | "http" (browser + dev server) | "none" (plain browser).
export const bridgeMode = () => bridge.mode;
// true when the bridge can reach the backend (native OR browser dev mode).
export const hasBridge = () => bridge.mode !== "none";
// true only inside the native host web view.
export const isNative = () => bridge.mode === "native";

// --- Bridge / diagnostics ---
export const ping = (text) => call("ping", text);

// { ok, appId, secure }  — `secure` is true when running a crypto-enabled host build.
export const appInfo = () => call("appInfo");

// --- Window control & links ---
// In the native host these drive the real window / OS shell. In the browser
// (dev mode or plain web) they degrade gracefully: fullscreen uses the DOM
// Fullscreen API (needs a user gesture), links open a new tab — which IS the
// user's default browser.

// Enter/leave native fullscreen. Resolves { ok, fullscreen }.
export const setFullscreen = (on = true) => {
  if (isNative()) return call("setFullscreen", Boolean(on));
  const p = on
    ? document.documentElement.requestFullscreen?.()
    : (document.fullscreenElement ? document.exitFullscreen?.() : undefined);
  return Promise.resolve(p)
    .then(() => ({ ok: true, fullscreen: Boolean(on) }))
    .catch((e) => ({ ok: false, error: String(e?.message ?? e) }));
};
// Resolves { ok, fullscreen }.
export const isFullscreen = () => {
  if (isNative()) return call("isFullscreen");
  return Promise.resolve({ ok: true, fullscreen: Boolean(document.fullscreenElement) });
};
export const toggleFullscreen = async () => {
  const cur = await isFullscreen();
  return setFullscreen(!cur.fullscreen);
};

// --- Clipboard (plain text). Native pasteboard in the host; in a browser it
// falls back to navigator.clipboard (permission rules apply there).
export const clipboard = {
  // -> { ok, text } (text is null when the clipboard is empty / has no text)
  readText: async () => {
    if (isNative()) return call("clipboardReadText");
    try {
      const text = await navigator.clipboard.readText();
      return { ok: true, text };
    } catch (e) { return { ok: false, error: String(e?.message ?? e) }; }
  },
  writeText: async (text) => {
    if (isNative()) return call("clipboardWriteText", String(text ?? ""));
    try {
      await navigator.clipboard.writeText(String(text ?? ""));
      return { ok: true };
    } catch (e) { return { ok: false, error: String(e?.message ?? e) }; }
  },
};

// --- Native dialogs (host window only). All modal.
export const dialogs = {
  // open({ title?, directory?, multiple?, filters?: [{name, extensions}] })
  //   -> { ok, canceled, paths: string[] }
  open: (options = {}) => call("dialogOpen", options),
  // save({ title?, defaultName?, filters? }) -> { ok, canceled, path: string|null }
  save: (options = {}) => call("dialogSave", options),
  // message({ message, title?, detail?, buttons?, type?, defaultId? })
  //   -> { ok, button: index } (Windows maps buttons to its fixed OK/Cancel/Yes/No sets)
  message: (options = {}) => call("dialogMessage", options),
};

// --- Tray / status item (Windows + macOS; Linux replies { ok: false } — no
// StatusNotifierItem host in Hull yet). Menu clicks arrive as bridge events:
//   bridge.on("tray:menu", ({ id }) => ...)   // a menu item was chosen
//   bridge.on("tray:click", () => ...)        // icon left-click (Windows only)
export const tray = {
  // set({ tooltip?, icon?, menu?: [{id, label, type?: "normal"|"separator"|"checkbox",
  //        checked?, enabled?}] }) -> { ok }
  set: (options = {}) => call("traySet", options),
  remove: () => call("trayRemove"),
};

// --- Local-path shell operations (paths usually come from the dialogs above).
export const openPath = (path) => call("openPath", String(path));     // OS default app
export const revealPath = (path) => call("revealPath", String(path)); // show in file manager
export const trashPath = (path) => call("trashPath", String(path));   // move to trash/recycle bin

// Grouped native-window control (native host only — replies { ok: false } with
// an explanation in browser dev mode). center/setAlwaysOnTop/setPosition are
// unsupported on Linux/Wayland (the compositor owns placement) and say so.
export const appWindow = {
  setFullscreen: (on = true) => setFullscreen(on),
  isFullscreen: () => isFullscreen(),
  toggleFullscreen: () => toggleFullscreen(),
  minimize: () => call("windowMinimize"),
  // maximize(true|false) — true maximizes, false restores
  maximize: (on = true) => call("windowMaximize", Boolean(on)),
  isMaximized: () => call("windowIsMaximized"),
  show: () => call("windowShow"),
  hide: () => call("windowHide"),
  center: () => call("windowCenter"),
  setAlwaysOnTop: (on = true) => call("windowSetAlwaysOnTop", Boolean(on)),
  setSize: (width, height) => call("windowSetSize", Number(width), Number(height)),
  setPosition: (x, y) => call("windowSetPosition", Number(x), Number(y)),
  // -> { ok, x?, y?, width, height, maximized, fullscreen } (x/y absent on Wayland)
  getBounds: () => call("windowGetBounds"),
};

// System notification (Windows toast/balloon, macOS Notification Center,
// Linux D-Bus). Title defaults to the app title when empty. In the browser
// (dev mode / plain web) this uses the Web Notification API, which needs the
// user's permission — request it from a click handler for best results.
export const notify = async (title, body = "") => {
  if (isNative()) return call("notify", String(title ?? ""), String(body));
  if (typeof Notification === "undefined") {
    return { ok: false, error: "notifications are not supported in this browser" };
  }
  let permission = Notification.permission;
  if (permission === "default") permission = await Notification.requestPermission();
  if (permission !== "granted") {
    return { ok: false, error: `notification permission ${permission}` };
  }
  new Notification(String(title ?? ""), { body: String(body) });
  return { ok: true };
};

// Open a URL with the OS default handler (http/https/mailto/tel — the default
// for every external link the app renders; see the link policy in features.md).
export const openExternal = (url) => {
  if (isNative()) return call("openExternal", String(url));
  window.open(String(url), "_blank", "noopener");
  return Promise.resolve({ ok: true, url: String(url) });
};
// Opt-in: open web content (http/https) in a NEW Hull window — a plain web view
// with NO bridge bindings. Same as clicking <a data-hull-window href="...">.
export const openWindow = (url, options = {}) => {
  if (isNative()) return call("openWindow", String(url), options);
  window.open(String(url), "_blank", "noopener");
  return Promise.resolve({ ok: true, url: String(url) });
};

// --- HTTP (TLS, on a C++ worker thread; auth token injected from the keychain) ---
export const httpPost = (url, body) => call("httpPost", url, body);
export const httpGet = (url) => call("httpGet", url);

export const http = {
  // Full request API: any verb, custom headers, JSON or raw-string bodies,
  // multipart forms. -> { ok, status, headers, body }
  // request({ url, method?, headers?, body?, form?, timeoutMs?, auth?, followRedirects? })
  //   body: object/array -> sent as application/json; string -> raw (set a
  //         Content-Type header). auth: false disables the keychain Bearer token.
  //   form: [{name, value} | {name, fileName, contentBase64, contentType?}] (POST)
  request: (options) => call("httpRequest", options),
  get: (url, options = {}) => call("httpRequest", { ...options, url, method: "GET" }),
  post: (url, body, options = {}) => call("httpRequest", { ...options, url, method: "POST", body }),
  put: (url, body, options = {}) => call("httpRequest", { ...options, url, method: "PUT", body }),
  patch: (url, body, options = {}) => call("httpRequest", { ...options, url, method: "PATCH", body }),
  delete: (url, options = {}) => call("httpRequest", { ...options, url, method: "DELETE" }),
  // Stream a URL to an absolute path (usually from dialogs.save). Progress
  // arrives as bridge events: bridge.on("http:download", ({received, total}) => …)
  // -> { ok, path, bytes }
  download: (options) => call("httpDownload", options),
};

// --- Assisted updates (opt-in). Hull doesn't self-patch binaries; the flow is
// check -> download installer -> hand it to the user/OS. `hull build` emits an
// update-manifest.json next to the release archives you can host anywhere.
export const updates = {
  // check(manifestUrl, currentVersion) ->
  //   { ok, updateAvailable, latest: <manifest>, currentVersion }
  async check(manifestUrl, currentVersion) {
    const res = await http.get(String(manifestUrl), { auth: false });
    if (!res?.ok) return { ok: false, error: res?.error ?? `HTTP ${res?.status}` };
    const manifest = res.body && typeof res.body === "object" ? res.body : null;
    if (!manifest?.version) return { ok: false, error: "invalid update manifest (no version)" };
    return {
      ok: true,
      updateAvailable: compareVersions(manifest.version, currentVersion) > 0,
      latest: manifest,
      currentVersion: String(currentVersion ?? ""),
    };
  },
  // Download an update artifact to a user-chosen path (pair with dialogs.save).
  // Progress: bridge.on("http:download", ...). -> { ok, path, bytes }
  download: (url, path, options = {}) => http.download({ ...options, url, path, auth: false }),
  // Open the downloaded installer/archive with the OS (installer runs, archive opens).
  openInstaller: (path) => openPath(path),
};

// --- OS theme (dark/light). The embedded web views follow the OS theme, so
// matchMedia is the source of truth — works in the host AND the browser.
export const getTheme = () =>
  typeof window !== "undefined" &&
  window.matchMedia?.("(prefers-color-scheme: dark)").matches ? "dark" : "light";
// Calls cb("dark"|"light") on every OS theme change; returns an unsubscribe fn.
export const onThemeChanged = (cb) => {
  if (typeof window === "undefined" || !window.matchMedia) return () => {};
  const mq = window.matchMedia("(prefers-color-scheme: dark)");
  const handler = (e) => cb(e.matches ? "dark" : "light");
  mq.addEventListener("change", handler);
  return () => mq.removeEventListener("change", handler);
};

// --- Settings (persisted + AES-256-GCM encrypted at rest) ---
export const saveSetting = (key, value) => call("saveSetting", key, value);
export const loadSetting = (key) => call("loadSetting", key);
export const loadAllSettings = () => call("loadAllSettings");

// --- Credentials (WRITE-ONLY from the UI; secrets never returned to JS) ---
export const saveCredential = (service, account, secret) =>
  call("saveCredential", service, account, secret);
export const credentialExists = (service, account) =>
  call("credentialExists", service, account);
export const eraseCredential = (service, account) =>
  call("eraseCredential", service, account);

// --- Printing (Winspool / CUPS) ---
// printMessage: text document — works with ANY printer (Print to PDF, OneNote, laser).
// printReceipt / printNetwork: raw ESC/POS for thermal receipt printers (spooler / TCP).
export const listPrinters = () => call("listPrinters");
export const printMessage = (printer, text) => call("printMessage", printer, text);
export const printReceipt = (printer, text) => call("printReceipt", printer, text);
export const printNetwork = (host, port, text) => call("printNetwork", host, port, text);

// --- SQLite (parameterized; stored in the per-user app dir) ---
// Ergonomic wrapper: unwraps the bridge envelope and throws on error, so you can
// use plain try/catch. Always pass values via the `params` array (never string-
// concatenate) — they're bound in C++, which is what makes it injection-safe.
async function dbCall(method, ...args) {
  const res = await call(method, ...args);
  if (!res?.ok) throw new Error(res?.error ?? `${method} failed`);
  return res;
}

export const db = {
  // INSERT/UPDATE/DELETE/DDL (one statement). -> { changes, lastInsertRowid }
  async exec(sql, params = []) {
    const r = await dbCall("dbExec", sql, params);
    return { changes: r.changes, lastInsertRowid: r.lastInsertRowid };
  },
  // SELECT -> array of row objects
  async query(sql, params = []) {
    return (await dbCall("dbQuery", sql, params)).rows;
  },
  // SELECT first row -> row object or null
  async get(sql, params = []) {
    return (await dbCall("dbGet", sql, params)).row;
  },
  // Run several { sql, params } atomically (one transaction). -> results[]
  async batch(statements) {
    return (await dbCall("dbBatch", statements)).results;
  },
  // Queue several statements and run them as ONE atomic transaction (batch
  // sugar). The builder is synchronous — tx.exec() queues, it does not read;
  // for read-modify-write logic use db.query first, then a transaction.
  //   await db.transaction((tx) => {
  //     tx.exec("UPDATE accounts SET balance = balance - ? WHERE id = ?", [50, a]);
  //     tx.exec("UPDATE accounts SET balance = balance + ? WHERE id = ?", [50, b]);
  //   });
  async transaction(build) {
    const stmts = [];
    build({ exec: (sql, params = []) => { stmts.push({ sql, params }); } });
    if (!stmts.length) return [];
    return (await dbCall("dbBatch", stmts)).results;
  },
  // Consistent online snapshot of the whole database to an absolute path
  // (usually from dialogs.save). Fails if the destination already exists.
  async backup(path) {
    await dbCall("dbBackup", String(path));
    return path;
  },
  // Apply ordered, run-once migrations. `steps` is an array of SQL strings (or
  // { sql }); step index i is schema version i+1, tracked via PRAGMA user_version.
  async migrate(steps) {
    const row = await this.get("PRAGMA user_version");
    const current = row?.user_version ?? 0;
    if (current >= steps.length) return current;
    const stmts = [];
    for (let i = current; i < steps.length; i++) {
      stmts.push({ sql: typeof steps[i] === "string" ? steps[i] : steps[i].sql });
    }
    stmts.push({ sql: `PRAGMA user_version = ${steps.length}` });
    await this.batch(stmts);
    return steps.length;
  },
};

// --- Files (uploads/blobs; stored per-user; passed through the secure layer) ---
function bytesToBase64(bytes) {
  let bin = "";
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
  return btoa(bin);
}
function base64ToBytes(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export const files = {
  // content: string (UTF-8) | Uint8Array | ArrayBuffer | Blob
  async write(name, content) {
    let bytes;
    if (typeof content === "string") bytes = new TextEncoder().encode(content);
    else if (content instanceof Uint8Array) bytes = content;
    else if (content instanceof ArrayBuffer) bytes = new Uint8Array(content);
    else if (typeof Blob !== "undefined" && content instanceof Blob)
      bytes = new Uint8Array(await content.arrayBuffer());
    else throw new Error("files.write: content must be a string, Uint8Array, ArrayBuffer, or Blob");
    const res = await call("fileWrite", name, bytesToBase64(bytes));
    if (!res?.ok) throw new Error(res?.error ?? "fileWrite failed");
  },
  async read(name) {
    const res = await call("fileRead", name);
    if (!res?.ok) throw new Error(res?.error ?? "fileRead failed");
    return base64ToBytes(res.data); // Uint8Array
  },
  async readText(name) {
    return new TextDecoder().decode(await this.read(name));
  },
  async list() {
    const res = await call("fileList");
    if (!res?.ok) throw new Error(res?.error ?? "fileList failed");
    return res.files; // [{ name, size }]
  },
  async remove(name) {
    const res = await call("fileDelete", name);
    if (!res?.ok) throw new Error(res?.error ?? "fileDelete failed");
    return res.removed;
  },
  // Path-based IO for files the user picked via dialogs.open()/save() —
  // absolute paths, raw bytes (not the managed per-user store above).
  async readAt(path) {
    const res = await call("fileReadAt", String(path));
    if (!res?.ok) throw new Error(res?.error ?? "fileReadAt failed");
    return base64ToBytes(res.data); // Uint8Array
  },
  async readTextAt(path) {
    return new TextDecoder().decode(await this.readAt(path));
  },
  async writeAt(path, content) {
    let bytes;
    if (typeof content === "string") bytes = new TextEncoder().encode(content);
    else if (content instanceof Uint8Array) bytes = content;
    else if (content instanceof ArrayBuffer) bytes = new Uint8Array(content);
    else if (typeof Blob !== "undefined" && content instanceof Blob)
      bytes = new Uint8Array(await content.arrayBuffer());
    else throw new Error("files.writeAt: content must be a string, Uint8Array, ArrayBuffer, or Blob");
    const res = await call("fileWriteAt", String(path), bytesToBase64(bytes));
    if (!res?.ok) throw new Error(res?.error ?? "fileWriteAt failed");
  },
};
