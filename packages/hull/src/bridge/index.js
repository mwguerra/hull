// Public bridge API for app code:  import { ping, httpPost, ... } from "@mwguerra/hull/bridge";
//
// Every call goes UI -> C++ and returns a Promise. All real work (TLS HTTP,
// encrypted storage, keychain, printing) happens in the native host.

import { bridge } from "./bridge-core.js";

export { bridge } from "./bridge-core.js";
export { nativeSetting } from "./native-store.js";

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

// --- HTTP (TLS, on a C++ worker thread; auth token injected from the keychain) ---
export const httpPost = (url, body) => call("httpPost", url, body);
export const httpGet = (url) => call("httpGet", url);

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
};
