import "./style.css";
import {
  ping,
  httpPost,
  httpGet,
  listPrinters,
  printMessage,
  printReceipt,
  printNetwork,
  saveSetting,
  loadSetting,
  loadAllSettings,
  saveCredential,
  credentialExists,
  eraseCredential,
  nativeSetting,
  db,
  files,
  appInfo,
  bridge,
  isNative,
  hasBridge,
  bridgeMode,
} from "@mwguerra/hull/bridge";
import logoUrl from "./assets/hull-logo.svg";

const $ = (id) => document.getElementById(id);
const show = (el, on = true) => { el.hidden = !on; };

// --- header logo + badge ---
$("logo").src = logoUrl;
{
  const b = $("native-badge");
  const labels = { native: "native host", http: "browser + bridge", none: "browser" };
  b.textContent = labels[bridgeMode()];
  b.className = "badge " + (hasBridge() ? "ok" : "no");
}

// 1) ping
$("ping-send").addEventListener("click", async () => {
  show($("ping-out"), false); show($("ping-err"), false);
  try {
    const res = await ping($("ping-text").value);
    $("ping-out").textContent = JSON.stringify(res, null, 2);
    show($("ping-out"));
  } catch (e) {
    $("ping-err").textContent = e.message;
    show($("ping-err"));
  }
});

// 2) settings — theme via the two-way store (Layer 2), used directly here
const theme = nativeSetting("theme");
const applyTheme = (v) => document.documentElement.classList.toggle("dark", v === "dark");
theme.subscribe(applyTheme);            // C++ -> UI (and local notifications)
theme.load().then(applyTheme).catch(() => {}); // initial pull (no-op in a plain browser)
$("theme").addEventListener("change", (e) => {
  theme.set(e.target.value).catch((err) => console.error(err));
});

$("note-save").addEventListener("click", async () => {
  try {
    const res = await saveSetting($("note-key").value, $("note-val").value);
    $("note-status").textContent = res?.ok ? "saved (encrypted at rest)" : res?.error ?? "failed";
  } catch (e) {
    $("note-status").textContent = e.message;
  }
  show($("note-status"));
});

$("settings-reload").addEventListener("click", async () => {
  try {
    const res = await loadAllSettings();
    $("settings-all").textContent = JSON.stringify(res?.ok ? res.value : res, null, 2);
  } catch (e) {
    $("settings-all").textContent = e.message;
  }
  show($("settings-all"));
});

// C++ -> UI push events
const log = $("event-log");
bridge.on("settings:changed", (p) => {
  if (log.querySelector(".muted")) log.innerHTML = "";
  const li = document.createElement("li");
  li.textContent = `${new Date().toLocaleTimeString()} · ${p.key} = ${JSON.stringify(p.value)}`;
  log.prepend(li);
  while (log.children.length > 8) log.removeChild(log.lastChild);
});

// 3) credentials
const credStatus = (msg) => { $("cred-status").textContent = msg; show($("cred-status")); };
$("cred-store").addEventListener("click", async () => {
  try {
    const res = await saveCredential($("cred-service").value, $("cred-account").value, $("cred-secret").value);
    $("cred-secret").value = ""; // don't keep the secret in the DOM
    credStatus(res?.ok ? "stored in OS keychain" : res?.error ?? "failed");
  } catch (e) { credStatus(e.message); }
});
$("cred-check").addEventListener("click", async () => {
  try {
    const res = await credentialExists($("cred-service").value, $("cred-account").value);
    credStatus(`exists: ${res?.exists}`);
  } catch (e) { credStatus(e.message); }
});
$("cred-remove").addEventListener("click", async () => {
  try {
    const res = await eraseCredential($("cred-service").value, $("cred-account").value);
    credStatus(res?.ok ? "removed" : res?.error ?? "failed");
  } catch (e) { credStatus(e.message); }
});

// 4) http — POST and GET both run in C++ on a worker thread
async function httpCall(btn, fn) {
  btn.disabled = true;
  show($("http-out"), false);
  try {
    const res = await fn();
    $("http-out").textContent = JSON.stringify(res, null, 2);
  } catch (e) {
    $("http-out").textContent = e.message;
  } finally {
    btn.disabled = false;
    show($("http-out"));
  }
}
$("http-send").addEventListener("click", () => httpCall($("http-send"), () => {
  let body;
  try { body = JSON.parse($("http-body").value); } catch { body = $("http-body").value; }
  return httpPost($("http-url").value, body);
}));
$("http-get").addEventListener("click", () =>
  httpCall($("http-get"), () => httpGet($("http-url").value)));

// 5) printers
$("pr-discover").addEventListener("click", async () => {
  try {
    const res = await listPrinters();
    const sel = $("pr-select");
    sel.innerHTML = "";
    const printers = res?.printers ?? [];
    if (!printers.length) sel.innerHTML = '<option value="">no printers found</option>';
    for (const p of printers) {
      const opt = document.createElement("option");
      opt.value = p.name;
      opt.textContent = p.name + (p.isDefault ? " (default)" : "");
      if (p.isDefault) opt.selected = true;
      sel.appendChild(opt);
    }
  } catch (e) {
    $("pr-status").textContent = e.message; show($("pr-status"));
  }
});
$("pr-print").addEventListener("click", async () => {
  try {
    const res = await printMessage($("pr-select").value, $("pr-text").value);
    $("pr-status").textContent = res?.ok ? "sent to spooler" : res?.error ?? "print failed";
  } catch (e) {
    $("pr-status").textContent = e.message;
  }
  show($("pr-status"));
});
// raw ESC/POS through the spooler — for thermal receipt printers
$("pr-receipt").addEventListener("click", async () => {
  try {
    const res = await printReceipt($("pr-select").value, $("pr-text").value);
    $("pr-status").textContent = res?.ok ? "receipt sent (ESC/POS)" : res?.error ?? "print failed";
  } catch (e) {
    $("pr-status").textContent = e.message;
  }
  show($("pr-status"));
});
// raw ESC/POS straight to a network printer on TCP port 9100
$("pr-net").addEventListener("click", async () => {
  const host = $("pr-host").value.trim();
  if (!host) { $("pr-status").textContent = "enter the printer IP first"; show($("pr-status")); return; }
  try {
    const res = await printNetwork(host, Number($("pr-port").value) || 9100, $("pr-text").value);
    $("pr-status").textContent = res?.ok ? `receipt sent to ${host}` : res?.error ?? "print failed";
  } catch (e) {
    $("pr-status").textContent = e.message;
  }
  show($("pr-status"));
});

// 6) SQLite — a tiny notes app (migrate once, then CRUD), persisted on disk
const dbErr = (msg) => { $("db-error").textContent = msg; show($("db-error")); };
async function loadNotes() {
  try {
    // db.get -> single row (here: the live count shown above the list)
    const count = await db.get("SELECT COUNT(*) AS n FROM notes");
    $("note-count").textContent = `${count?.n ?? 0} note(s) — counted via db.get`;
    show($("note-count"));
    const rows = await db.query("SELECT id, body FROM notes ORDER BY id DESC");
    const ul = $("notes");
    ul.innerHTML = "";
    if (!rows.length) { ul.innerHTML = '<li class="muted">no notes yet</li>'; return; }
    for (const n of rows) {
      const li = document.createElement("li");
      li.style.cssText = "display:flex;justify-content:space-between;gap:.5rem";
      const span = document.createElement("span");
      span.textContent = n.body;
      const a = document.createElement("a");
      a.href = "#"; a.className = "muted"; a.textContent = "delete";
      a.addEventListener("click", async (e) => { e.preventDefault(); await deleteNote(n.id); });
      li.append(span, a);
      ul.appendChild(li);
    }
  } catch (e) { dbErr(e.message); }
}
async function addNote() {
  const body = $("note-body").value.trim();
  if (!body) return;
  try { await db.exec("INSERT INTO notes (body) VALUES (?)", [body]); $("note-body").value = ""; await loadNotes(); }
  catch (e) { dbErr(e.message); }
}
async function deleteNote(id) {
  try { await db.exec("DELETE FROM notes WHERE id = ?", [id]); await loadNotes(); }
  catch (e) { dbErr(e.message); }
}
$("note-add").addEventListener("click", addNote);
$("note-body").addEventListener("keyup", (e) => { if (e.key === "Enter") addNote(); });
// db.batch -> several statements in ONE atomic transaction
$("note-batch").addEventListener("click", async () => {
  try {
    await db.batch([
      { sql: "INSERT INTO notes (body) VALUES (?)", params: ["Sample: bridge calls run in C++"] },
      { sql: "INSERT INTO notes (body) VALUES (?)", params: ["Sample: SQLite is parameterized"] },
      { sql: "INSERT INTO notes (body) VALUES (?)", params: ["Sample: batch = one transaction"] },
    ]);
    await loadNotes();
  } catch (e) { dbErr(e.message); }
});

// 7) Files — store/list/read/delete uploads in the per-user dir
const fileErr = (msg) => { $("file-error").textContent = msg; show($("file-error")); };
async function loadFiles() {
  try {
    const list = await files.list();
    const ul = $("files");
    ul.innerHTML = "";
    if (!list.length) { ul.innerHTML = '<li class="muted">no files yet</li>'; return; }
    for (const f of list) {
      const li = document.createElement("li");
      li.style.cssText = "display:flex;justify-content:space-between;gap:.5rem";
      const span = document.createElement("span");
      span.textContent = `${f.name} (${f.size} B)`;
      const actions = document.createElement("span");
      const view = document.createElement("a");
      view.href = "#"; view.className = "muted"; view.textContent = "view";
      view.addEventListener("click", async (e) => { e.preventDefault(); await viewFile(f.name); });
      const del = document.createElement("a");
      del.href = "#"; del.className = "muted"; del.textContent = "delete";
      del.addEventListener("click", async (e) => { e.preventDefault(); await removeFile(f.name); });
      actions.append(view, document.createTextNode(" · "), del);
      li.append(span, actions);
      ul.appendChild(li);
    }
  } catch (e) { fileErr(e.message); }
}
async function viewFile(name) {
  let text;
  try { text = await files.readText(name); } catch { text = "(binary or unreadable)"; }
  $("file-preview").textContent = `${name}:\n${text}`;
  show($("file-preview"));
}
async function removeFile(name) {
  try { await files.remove(name); show($("file-preview"), false); await loadFiles(); }
  catch (e) { fileErr(e.message); }
}
$("file-input").addEventListener("change", async (e) => {
  const f = e.target.files?.[0];
  if (!f) return;
  try { await files.write(f.name, f); await loadFiles(); } catch (err) { fileErr(err.message); }
  e.target.value = "";
});

// 8) Image — a single uploaded image. Uploading a new one deletes the previous;
// the stored name is tracked in a setting so the preview survives restarts.
const MIME = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", svg: "image/svg+xml", bmp: "image/bmp" };
let imageName = null;
let imageObjUrl = null;
const imageErr = (msg) => { $("image-error").textContent = msg; show($("image-error")); };
function renderImage(bytes, name) {
  const mime = MIME[name.split(".").pop()?.toLowerCase()] ?? "application/octet-stream";
  if (imageObjUrl) URL.revokeObjectURL(imageObjUrl);
  imageObjUrl = URL.createObjectURL(new Blob([bytes], { type: mime }));
  imageName = name;
  $("image-preview").src = imageObjUrl;
  $("image-preview").alt = name;
  $("image-name").textContent = name;
  show($("image-box"), true);
  show($("image-empty"), false);
}
function clearImage() {
  if (imageObjUrl) { URL.revokeObjectURL(imageObjUrl); imageObjUrl = null; }
  imageName = null;
  $("image-preview").removeAttribute("src");
  show($("image-box"), false);
  show($("image-empty"), true);
}
async function showStoredImage(name) {
  try { renderImage(await files.read(name), name); } catch (e) { imageErr(e.message); }
}
$("image-input").addEventListener("change", async (e) => {
  const f = e.target.files?.[0];
  e.target.value = "";
  if (!f) return;
  show($("image-error"), false);
  try {
    if (imageName && imageName !== f.name) { try { await files.remove(imageName); } catch {} }
    await files.write(f.name, f);
    await saveSetting("uploadedImage", f.name);
    renderImage(new Uint8Array(await f.arrayBuffer()), f.name);
    await loadFiles();
  } catch (err) { imageErr(err.message); }
});
$("image-delete").addEventListener("click", async () => {
  if (!imageName) return;
  try {
    await files.remove(imageName);
    await saveSetting("uploadedImage", "");
    clearImage();
    await loadFiles();
  } catch (e) { imageErr(e.message); }
});

// init backend-backed sections (native host or browser dev mode)
if (hasBridge()) {
  appInfo().then((info) => { if (info?.secure) show($("secure-badge")); }).catch(() => {});
  db.migrate([
    "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')))",
  ]).then(loadNotes).catch((e) => dbErr(e.message));
  loadFiles();
  loadSetting("uploadedImage")
    .then((s) => { if (s?.ok && s.value) return showStoredImage(s.value); })
    .catch(() => {});
}
