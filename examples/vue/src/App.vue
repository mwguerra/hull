<script setup>
import { ref, reactive, watch, onMounted, onUnmounted } from "vue";
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
  db,
  files,
  appInfo,
  bridge,
  isNative,
  hasBridge,
  bridgeMode,
} from "@mwguerra/hull/bridge";
import { useNativeState } from "@mwguerra/hull/vue";
import logoUrl from "./assets/hull-logo.svg";

const modeLabel = { native: "native host", http: "browser + bridge", none: "browser" }[bridgeMode()];
const secure = ref(false);

// 1) ping
const pingText = ref("Hello from Vue");
const pingOut = ref(null);
const pingErr = ref(null);
async function doPing() {
  pingErr.value = null; pingOut.value = null;
  try { pingOut.value = await ping(pingText.value); }
  catch (e) { pingErr.value = e.message; }
}

// 2) settings (two-way, encrypted at rest)
const theme = useNativeState("theme");
watch(theme, (v) => document.documentElement.classList.toggle("dark", v === "dark"), { immediate: true });

const noteKey = ref("note");
const noteVal = ref("");
const noteStatus = ref(null);
async function saveNote() {
  try {
    const res = await saveSetting(noteKey.value, noteVal.value);
    noteStatus.value = res?.ok ? "saved (encrypted at rest)" : res?.error ?? "failed";
  } catch (e) { noteStatus.value = e.message; }
}

const allSettings = ref(null);
async function reloadSettings() {
  try { const res = await loadAllSettings(); allSettings.value = res?.ok ? res.value : res; }
  catch (e) { allSettings.value = { error: e.message }; }
}

const events = reactive([]);
let unsub = null;
onMounted(() => {
  unsub = bridge.on("settings:changed", (p) => {
    events.unshift({ t: new Date().toLocaleTimeString(), ...p });
    if (events.length > 8) events.pop();
  });
});
onUnmounted(() => unsub && unsub());

// 3) credentials
const cred = reactive({ service: "api.example.com", account: "default", secret: "" });
const credStatus = ref(null);
async function storeCred() {
  try {
    const res = await saveCredential(cred.service, cred.account, cred.secret);
    cred.secret = "";
    credStatus.value = res?.ok ? "stored in OS keychain" : res?.error ?? "failed";
  } catch (e) { credStatus.value = e.message; }
}
async function checkCred() {
  try { const res = await credentialExists(cred.service, cred.account); credStatus.value = `exists: ${res?.exists}`; }
  catch (e) { credStatus.value = e.message; }
}
async function removeCred() {
  try { const res = await eraseCredential(cred.service, cred.account); credStatus.value = res?.ok ? "removed" : res?.error ?? "failed"; }
  catch (e) { credStatus.value = e.message; }
}

// 4) http — POST and GET both run in C++ on a worker thread
const http = reactive({ url: "https://httpbin.org/anything", body: '{ "name": "Widget", "qty": 3 }' });
const httpOut = ref(null);
const httpBusy = ref(false);
async function httpCall(fn) {
  httpBusy.value = true; httpOut.value = null;
  try { httpOut.value = await fn(); }
  catch (e) { httpOut.value = { ok: false, error: e.message }; }
  finally { httpBusy.value = false; }
}
function doPost() {
  return httpCall(() => {
    let parsed; try { parsed = JSON.parse(http.body); } catch { parsed = http.body; }
    return httpPost(http.url, parsed);
  });
}
function doGet() { return httpCall(() => httpGet(http.url)); }

// 5) printers
const printers = ref([]);
const printer = ref("");
const printText = ref("Hello from Hull!");
const printStatus = ref(null);
async function discover() {
  try {
    const res = await listPrinters();
    printers.value = res?.printers ?? [];
    printer.value = printers.value.find((p) => p.isDefault)?.name ?? printers.value[0]?.name ?? "";
  } catch (e) { printStatus.value = e.message; }
}
async function testPrint() {
  try { const res = await printMessage(printer.value, printText.value); printStatus.value = res?.ok ? "sent to spooler" : res?.error ?? "print failed"; }
  catch (e) { printStatus.value = e.message; }
}
// raw ESC/POS through the spooler — for thermal receipt printers
async function testReceipt() {
  try { const res = await printReceipt(printer.value, printText.value); printStatus.value = res?.ok ? "receipt sent (ESC/POS)" : res?.error ?? "print failed"; }
  catch (e) { printStatus.value = e.message; }
}
// raw ESC/POS straight to a network printer on TCP port 9100
const netHost = ref("");
const netPort = ref("9100");
async function testNetwork() {
  if (!netHost.value.trim()) { printStatus.value = "enter the printer IP first"; return; }
  try {
    const res = await printNetwork(netHost.value.trim(), Number(netPort.value) || 9100, printText.value);
    printStatus.value = res?.ok ? `receipt sent to ${netHost.value.trim()}` : res?.error ?? "print failed";
  } catch (e) { printStatus.value = e.message; }
}

// 6) SQLite — a tiny notes app (migrate once, then CRUD), persisted on disk
const notes = ref([]);
const noteCount = ref(null);
const newNote = ref("");
const dbError = ref(null);
async function loadNotes() {
  try {
    // db.get -> single row (here: the live count shown above the list)
    const count = await db.get("SELECT COUNT(*) AS n FROM notes");
    noteCount.value = count?.n ?? 0;
    notes.value = await db.query("SELECT id, body FROM notes ORDER BY id DESC");
  } catch (e) { dbError.value = e.message; }
}
// db.batch -> several statements in ONE atomic transaction
async function addSamples() {
  try {
    await db.batch([
      { sql: "INSERT INTO notes (body) VALUES (?)", params: ["Sample: bridge calls run in C++"] },
      { sql: "INSERT INTO notes (body) VALUES (?)", params: ["Sample: SQLite is parameterized"] },
      { sql: "INSERT INTO notes (body) VALUES (?)", params: ["Sample: batch = one transaction"] },
    ]);
    await loadNotes();
  } catch (e) { dbError.value = e.message; }
}
async function addNote() {
  if (!newNote.value.trim()) return;
  try {
    await db.exec("INSERT INTO notes (body) VALUES (?)", [newNote.value.trim()]);
    newNote.value = "";
    await loadNotes();
  } catch (e) { dbError.value = e.message; }
}
async function deleteNote(id) {
  try { await db.exec("DELETE FROM notes WHERE id = ?", [id]); await loadNotes(); }
  catch (e) { dbError.value = e.message; }
}

// 7) Files — store/list/read/delete uploads in the per-user dir
const fileList = ref([]);
const filePreview = ref(null);
const fileError = ref(null);
async function loadFiles() {
  try { fileList.value = await files.list(); } catch (e) { fileError.value = e.message; }
}
async function pickFile(e) {
  const f = e.target.files?.[0];
  if (!f) return;
  try { await files.write(f.name, f); await loadFiles(); }
  catch (err) { fileError.value = err.message; }
  e.target.value = "";
}
async function viewFile(name) {
  try { filePreview.value = { name, text: await files.readText(name) }; }
  catch { filePreview.value = { name, text: "(binary or unreadable)" }; }
}
async function removeFile(name) {
  try { await files.remove(name); if (filePreview.value?.name === name) filePreview.value = null; await loadFiles(); }
  catch (e) { fileError.value = e.message; }
}

// 8) Image — a single uploaded image. Uploading a new one deletes the previous;
// the stored name is tracked in a setting so the preview survives restarts.
const imageName = ref(null);
const imageUrl = ref(null);
const imageError = ref(null);
const MIME = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", svg: "image/svg+xml", bmp: "image/bmp" };
function setImageUrl(bytes, name) {
  const mime = MIME[name.split(".").pop()?.toLowerCase()] ?? "application/octet-stream";
  if (imageUrl.value) URL.revokeObjectURL(imageUrl.value);
  imageUrl.value = URL.createObjectURL(new Blob([bytes], { type: mime }));
  imageName.value = name;
}
async function showImage(name) {
  try { setImageUrl(await files.read(name), name); }
  catch (e) { imageError.value = e.message; }
}
async function pickImage(e) {
  const f = e.target.files?.[0];
  e.target.value = "";
  if (!f) return;
  imageError.value = null;
  try {
    if (imageName.value && imageName.value !== f.name) {
      try { await files.remove(imageName.value); } catch {}
    }
    await files.write(f.name, f);
    await saveSetting("uploadedImage", f.name);
    setImageUrl(new Uint8Array(await f.arrayBuffer()), f.name);
    await loadFiles();
  } catch (err) { imageError.value = err.message; }
}
async function deleteImage() {
  if (!imageName.value) return;
  try {
    await files.remove(imageName.value);
    await saveSetting("uploadedImage", "");
    if (imageUrl.value) URL.revokeObjectURL(imageUrl.value);
    imageUrl.value = null; imageName.value = null;
    await loadFiles();
  } catch (e) { imageError.value = e.message; }
}

onMounted(async () => {
  if (!hasBridge()) return; // backend works in the native host or browser dev mode
  try {
    const info = await appInfo();
    secure.value = !!info?.secure;
    await db.migrate([
      "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')))",
    ]);
    await loadNotes();
    await loadFiles();
    const saved = await loadSetting("uploadedImage");
    if (saved?.ok && saved.value) await showImage(saved.value);
  } catch (e) { dbError.value = e.message; }
});
onUnmounted(() => { if (imageUrl.value) URL.revokeObjectURL(imageUrl.value); });
</script>

<template>
  <div class="wrap">
    <div class="top">
      <div class="brand">
        <img class="logo" :src="logoUrl" alt="Hull" />
        <div>
          <h1>Hull · Vue</h1>
          <div class="sub">A Vue app running as a native desktop window.</div>
        </div>
      </div>
      <span class="badge" :class="hasBridge() ? 'ok' : 'no'">{{ modeLabel }}</span>
      <span v-if="secure" class="badge ok">secure</span>
    </div>

    <section class="card">
      <h2>1 · Bridge call</h2>
      <p class="hint">Synchronous <code>ping(text)</code> → C++ echoes a JSON result.</p>
      <input v-model="pingText" />
      <div class="actions"><button @click="doPing">Send to C++</button></div>
      <pre v-if="pingOut">{{ pingOut }}</pre>
      <p v-if="pingErr" class="err">{{ pingErr }}</p>
    </section>

    <section class="card">
      <h2>2 · Settings (encrypted at rest)</h2>
      <p class="hint">Two-way state persisted by C++ (AES-256-GCM); C++ pushes a <code>settings:changed</code> event after every write.</p>
      <label>Theme</label>
      <select v-model="theme">
        <option :value="undefined" disabled>— choose a theme —</option>
        <option value="light">Light</option>
        <option value="dark">Dark</option>
      </select>
      <div class="grid2" style="margin-top:.6rem">
        <input v-model="noteKey" />
        <input v-model="noteVal" placeholder="value" />
      </div>
      <div class="actions">
        <button @click="saveNote">Save setting</button>
        <button class="ghost" @click="reloadSettings">Reload all</button>
      </div>
      <p v-if="noteStatus" class="status">{{ noteStatus }}</p>
      <pre v-if="allSettings">{{ allSettings }}</pre>
      <label style="margin-top:.8rem">C++ → UI events</label>
      <ul class="log">
        <li v-if="!events.length" class="muted">no events yet</li>
        <li v-for="(e, i) in events" :key="i"><span class="muted">{{ e.t }}</span> · {{ e.key }} = {{ JSON.stringify(e.value) }}</li>
      </ul>
    </section>

    <section class="card">
      <h2>3 · Credentials (write-only)</h2>
      <p class="hint">Stored in the OS keychain. Never returned to JS — only a boolean check.</p>
      <div class="grid2">
        <div><label>Service</label><input v-model="cred.service" /></div>
        <div><label>Account</label><input v-model="cred.account" /></div>
      </div>
      <label>Secret / token</label>
      <input v-model="cred.secret" type="password" placeholder="never returned to JS" />
      <div class="actions">
        <button @click="storeCred">Store</button>
        <button class="ghost" @click="checkCred">Check exists</button>
        <button class="ghost" @click="removeCred">Remove</button>
      </div>
      <p v-if="credStatus" class="status">{{ credStatus }}</p>
    </section>

    <section class="card">
      <h2>4 · HTTP (TLS, from C++)</h2>
      <p class="hint">Runs on a C++ worker thread (cpp-httplib + OpenSSL). Adds a keychain <code>Bearer</code> token if one exists for the host. <code>httpbin.org/anything</code> echoes both verbs.</p>
      <label>URL</label>
      <input v-model="http.url" />
      <label>JSON body (POST)</label>
      <textarea v-model="http.body" rows="3"></textarea>
      <div class="actions">
        <button @click="doPost" :disabled="httpBusy">{{ httpBusy ? "Sending…" : "POST" }}</button>
        <button class="ghost" @click="doGet" :disabled="httpBusy">GET</button>
      </div>
      <pre v-if="httpOut">{{ httpOut }}</pre>
    </section>

    <section class="card">
      <h2>5 · Printers</h2>
      <p class="hint">Discover (Winspool / CUPS), then print a test message — a text document that works with any printer, incl. Microsoft Print to PDF. Receipt buttons send raw ESC/POS for thermal printers (spooler or TCP port 9100).</p>
      <div class="actions"><button class="ghost" @click="discover">Discover printers</button></div>
      <label>Printer</label>
      <select v-model="printer">
        <option v-if="!printers.length" value="">— none —</option>
        <option v-for="p in printers" :key="p.name" :value="p.name">{{ p.name }}{{ p.isDefault ? " (default)" : "" }}</option>
      </select>
      <label>Message</label>
      <input v-model="printText" />
      <div class="actions">
        <button @click="testPrint" :disabled="!printer">Print test message</button>
        <button class="ghost" @click="testReceipt" :disabled="!printer">Print ESC/POS receipt</button>
      </div>
      <label>Network receipt printer (ESC/POS over TCP)</label>
      <div class="grid2">
        <input v-model="netHost" placeholder="printer IP, e.g. 192.168.0.50" />
        <input v-model="netPort" />
      </div>
      <div class="actions"><button class="ghost" @click="testNetwork">Print via TCP/9100</button></div>
      <p v-if="printStatus" class="status">{{ printStatus }}</p>
    </section>

    <section class="card">
      <h2>6 · Notes (SQLite)</h2>
      <p class="hint">Parameterized SQLite in the C++ backend, stored in the per-user app dir.
        <code>migrate</code> sets up the schema once; add/delete persist across restarts.</p>
      <div class="grid2">
        <input v-model="newNote" placeholder="Write a note…" @keyup.enter="addNote" />
        <button style="flex:0 0 auto" @click="addNote">Add</button>
      </div>
      <div class="actions"><button class="ghost" @click="addSamples">Add 3 samples (db.batch)</button></div>
      <p v-if="noteCount !== null" class="status">{{ noteCount }} note(s) — counted via db.get</p>
      <ul class="log">
        <li v-if="!notes.length" class="muted">no notes yet</li>
        <li v-for="n in notes" :key="n.id" style="display:flex;justify-content:space-between;gap:.5rem">
          <span>{{ n.body }}</span>
          <a href="#" class="muted" @click.prevent="deleteNote(n.id)">delete</a>
        </li>
      </ul>
      <p v-if="dbError" class="err">{{ dbError }}</p>
    </section>

    <section class="card">
      <h2>7 · Files (uploads)</h2>
      <p class="hint">Stored in the per-user app dir through the secure layer; names are
        sanitized (no path traversal). Pick a file to upload, then view or delete it.</p>
      <input type="file" @change="pickFile" />
      <ul class="log">
        <li v-if="!fileList.length" class="muted">no files yet</li>
        <li v-for="f in fileList" :key="f.name" style="display:flex;justify-content:space-between;gap:.5rem">
          <span>{{ f.name }} <span class="muted">({{ f.size }} B)</span></span>
          <span>
            <a href="#" class="muted" @click.prevent="viewFile(f.name)">view</a> ·
            <a href="#" class="muted" @click.prevent="removeFile(f.name)">delete</a>
          </span>
        </li>
      </ul>
      <pre v-if="filePreview">{{ filePreview.name }}:
{{ filePreview.text }}</pre>
      <p v-if="fileError" class="err">{{ fileError }}</p>
    </section>

    <section class="card">
      <h2>8 · Image upload (single)</h2>
      <p class="hint">Upload one image — it's stored through the secure file layer and
        shown below. Uploading another replaces (deletes) the previous one. The preview
        survives restarts.</p>
      <input type="file" accept="image/*" @change="pickImage" />
      <div v-if="imageUrl" class="imgbox">
        <img class="preview" :src="imageUrl" :alt="imageName" />
        <div class="imgmeta">
          <span class="muted">{{ imageName }}</span>
          <button class="ghost" @click="deleteImage">Delete image</button>
        </div>
      </div>
      <p v-else class="muted">no image uploaded yet</p>
      <p v-if="imageError" class="err">{{ imageError }}</p>
    </section>

    <footer>Built with @mwguerra/hull · no Electron, no bundled browser</footer>
  </div>
</template>
