import { useEffect, useState } from "react";
import {
  ping,
  httpPost,
  listPrinters,
  printMessage,
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
} from "@mwguerra/hull/bridge";
import { useNativeState } from "@mwguerra/hull/react";
import logoUrl from "./assets/hull-logo.svg";

export default function App() {
  const native = isNative();
  const modeLabel = native ? "native host" : hasBridge() ? "browser + bridge" : "browser";
  const [secure, setSecure] = useState(false);

  // 1) ping
  const [pingText, setPingText] = useState("Hello from React");
  const [pingOut, setPingOut] = useState(null);
  const [pingErr, setPingErr] = useState(null);
  const doPing = async () => {
    setPingErr(null); setPingOut(null);
    try { setPingOut(await ping(pingText)); }
    catch (e) { setPingErr(e.message); }
  };

  // 2) settings (two-way, encrypted at rest)
  const [theme, setTheme] = useNativeState("theme");
  useEffect(() => {
    document.documentElement.classList.toggle("dark", theme === "dark");
  }, [theme]);

  const [noteKey, setNoteKey] = useState("note");
  const [noteVal, setNoteVal] = useState("");
  const [noteStatus, setNoteStatus] = useState(null);
  const saveNote = async () => {
    try {
      const res = await saveSetting(noteKey, noteVal);
      setNoteStatus(res?.ok ? "saved (encrypted at rest)" : res?.error ?? "failed");
    } catch (e) { setNoteStatus(e.message); }
  };

  const [allSettings, setAllSettings] = useState(null);
  const reloadSettings = async () => {
    try { const res = await loadAllSettings(); setAllSettings(res?.ok ? res.value : res); }
    catch (e) { setAllSettings({ error: e.message }); }
  };

  const [events, setEvents] = useState([]);
  useEffect(() => bridge.on("settings:changed", (p) => {
    setEvents((prev) => [{ t: new Date().toLocaleTimeString(), ...p }, ...prev].slice(0, 8));
  }), []);

  // 3) credentials
  const [cred, setCred] = useState({ service: "api.example.com", account: "default", secret: "" });
  const [credStatus, setCredStatus] = useState(null);
  const setCredField = (k) => (e) => setCred((c) => ({ ...c, [k]: e.target.value }));
  const storeCred = async () => {
    try {
      const res = await saveCredential(cred.service, cred.account, cred.secret);
      setCred((c) => ({ ...c, secret: "" }));
      setCredStatus(res?.ok ? "stored in OS keychain" : res?.error ?? "failed");
    } catch (e) { setCredStatus(e.message); }
  };
  const checkCred = async () => {
    try { const res = await credentialExists(cred.service, cred.account); setCredStatus(`exists: ${res?.exists}`); }
    catch (e) { setCredStatus(e.message); }
  };
  const removeCred = async () => {
    try { const res = await eraseCredential(cred.service, cred.account); setCredStatus(res?.ok ? "removed" : res?.error ?? "failed"); }
    catch (e) { setCredStatus(e.message); }
  };

  // 4) http
  const [url, setUrl] = useState("https://httpbin.org/post");
  const [body, setBody] = useState('{ "name": "Widget", "qty": 3 }');
  const [httpOut, setHttpOut] = useState(null);
  const [httpBusy, setHttpBusy] = useState(false);
  const doPost = async () => {
    setHttpBusy(true); setHttpOut(null);
    try {
      let parsed; try { parsed = JSON.parse(body); } catch { parsed = body; }
      setHttpOut(await httpPost(url, parsed));
    } catch (e) { setHttpOut({ ok: false, error: e.message }); }
    finally { setHttpBusy(false); }
  };

  // 5) printers
  const [printers, setPrinters] = useState([]);
  const [printer, setPrinter] = useState("");
  const [printText, setPrintText] = useState("Hello from Hull!");
  const [printStatus, setPrintStatus] = useState(null);
  const discover = async () => {
    try {
      const res = await listPrinters();
      const list = res?.printers ?? [];
      setPrinters(list);
      setPrinter(list.find((p) => p.isDefault)?.name ?? list[0]?.name ?? "");
    } catch (e) { setPrintStatus(e.message); }
  };
  const testPrint = async () => {
    try { const res = await printMessage(printer, printText); setPrintStatus(res?.ok ? "sent to spooler" : res?.error ?? "print failed"); }
    catch (e) { setPrintStatus(e.message); }
  };

  // 6) SQLite — a tiny notes app (migrate once, then CRUD), persisted on disk
  const [notes, setNotes] = useState([]);
  const [newNote, setNewNote] = useState("");
  const [dbError, setDbError] = useState(null);
  const loadNotes = async () => {
    try { setNotes(await db.query("SELECT id, body FROM notes ORDER BY id DESC")); }
    catch (e) { setDbError(e.message); }
  };
  const addNote = async () => {
    if (!newNote.trim()) return;
    try {
      await db.exec("INSERT INTO notes (body) VALUES (?)", [newNote.trim()]);
      setNewNote("");
      await loadNotes();
    } catch (e) { setDbError(e.message); }
  };
  const deleteNote = async (id) => {
    try { await db.exec("DELETE FROM notes WHERE id = ?", [id]); await loadNotes(); }
    catch (e) { setDbError(e.message); }
  };

  // 7) Files — store/list/read/delete uploads in the per-user dir
  const [fileList, setFileList] = useState([]);
  const [filePreview, setFilePreview] = useState(null);
  const [fileError, setFileError] = useState(null);
  const loadFiles = async () => {
    try { setFileList(await files.list()); } catch (e) { setFileError(e.message); }
  };
  const pickFile = async (e) => {
    const f = e.target.files?.[0];
    if (!f) return;
    try { await files.write(f.name, f); await loadFiles(); } catch (err) { setFileError(err.message); }
    e.target.value = "";
  };
  const viewFile = async (name) => {
    try { setFilePreview({ name, text: await files.readText(name) }); }
    catch { setFilePreview({ name, text: "(binary or unreadable)" }); }
  };
  const removeFile = async (name) => {
    try { await files.remove(name); setFilePreview(null); await loadFiles(); } catch (e) { setFileError(e.message); }
  };

  // 8) Image — a single uploaded image. Uploading a new one deletes the previous;
  // the stored name is tracked in a setting so the preview survives restarts.
  const MIME = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", svg: "image/svg+xml", bmp: "image/bmp" };
  const [imageName, setImageName] = useState(null);
  const [imageUrl, setImageUrl] = useState(null);
  const [imageError, setImageError] = useState(null);
  const showImageUrl = (bytes, name) => {
    const mime = MIME[name.split(".").pop()?.toLowerCase()] ?? "application/octet-stream";
    setImageUrl((prev) => { if (prev) URL.revokeObjectURL(prev); return URL.createObjectURL(new Blob([bytes], { type: mime })); });
    setImageName(name);
  };
  const showImage = async (name) => {
    try { showImageUrl(await files.read(name), name); } catch (e) { setImageError(e.message); }
  };
  const pickImage = async (e) => {
    const f = e.target.files?.[0];
    e.target.value = "";
    if (!f) return;
    setImageError(null);
    try {
      if (imageName && imageName !== f.name) { try { await files.remove(imageName); } catch {} }
      await files.write(f.name, f);
      await saveSetting("uploadedImage", f.name);
      showImageUrl(new Uint8Array(await f.arrayBuffer()), f.name);
      await loadFiles();
    } catch (err) { setImageError(err.message); }
  };
  const deleteImage = async () => {
    if (!imageName) return;
    try {
      await files.remove(imageName);
      await saveSetting("uploadedImage", "");
      setImageUrl((prev) => { if (prev) URL.revokeObjectURL(prev); return null; });
      setImageName(null);
      await loadFiles();
    } catch (e) { setImageError(e.message); }
  };

  useEffect(() => {
    if (!hasBridge()) return; // backend works in the native host or browser dev mode
    (async () => {
      try {
        const info = await appInfo();
        setSecure(!!info?.secure);
        await db.migrate([
          "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL, created_at TEXT DEFAULT (datetime('now')))",
        ]);
        await loadNotes();
        await loadFiles();
        const saved = await loadSetting("uploadedImage");
        if (saved?.ok && saved.value) await showImage(saved.value);
      } catch (e) { setDbError(e.message); }
    })();
  }, []);

  return (
    <div className="wrap">
      <div className="top">
        <div className="brand">
          <img className="logo" src={logoUrl} alt="Hull" />
          <div>
            <h1>Hull · React</h1>
            <div className="sub">A React app running as a native desktop window.</div>
          </div>
        </div>
        <span className={`badge ${hasBridge() ? "ok" : "no"}`}>{modeLabel}</span>
        {secure && <span className="badge ok">secure</span>}
      </div>

      <section className="card">
        <h2>1 · Bridge call</h2>
        <p className="hint">Synchronous <code>ping(text)</code> → C++ echoes a JSON result.</p>
        <input value={pingText} onChange={(e) => setPingText(e.target.value)} />
        <div className="actions"><button onClick={doPing}>Send to C++</button></div>
        {pingOut && <pre>{JSON.stringify(pingOut, null, 2)}</pre>}
        {pingErr && <p className="err">{pingErr}</p>}
      </section>

      <section className="card">
        <h2>2 · Settings (encrypted at rest)</h2>
        <p className="hint">Two-way state persisted by C++ (AES-256-GCM); C++ pushes a <code>settings:changed</code> event after every write.</p>
        <label>Theme</label>
        <select value={theme ?? ""} onChange={(e) => setTheme(e.target.value)}>
          <option value="" disabled>— choose a theme —</option>
          <option value="light">Light</option>
          <option value="dark">Dark</option>
        </select>
        <div className="grid2" style={{ marginTop: ".6rem" }}>
          <input value={noteKey} onChange={(e) => setNoteKey(e.target.value)} />
          <input value={noteVal} placeholder="value" onChange={(e) => setNoteVal(e.target.value)} />
        </div>
        <div className="actions">
          <button onClick={saveNote}>Save setting</button>
          <button className="ghost" onClick={reloadSettings}>Reload all</button>
        </div>
        {noteStatus && <p className="status">{noteStatus}</p>}
        {allSettings && <pre>{JSON.stringify(allSettings, null, 2)}</pre>}
        <label style={{ marginTop: ".8rem" }}>C++ → UI events</label>
        <ul className="log">
          {events.length === 0 && <li className="muted">no events yet</li>}
          {events.map((e, i) => <li key={i}><span className="muted">{e.t}</span> · {e.key} = {JSON.stringify(e.value)}</li>)}
        </ul>
      </section>

      <section className="card">
        <h2>3 · Credentials (write-only)</h2>
        <p className="hint">Stored in the OS keychain. Never returned to JS — only a boolean check.</p>
        <div className="grid2">
          <div><label>Service</label><input value={cred.service} onChange={setCredField("service")} /></div>
          <div><label>Account</label><input value={cred.account} onChange={setCredField("account")} /></div>
        </div>
        <label>Secret / token</label>
        <input type="password" value={cred.secret} placeholder="never returned to JS" onChange={setCredField("secret")} />
        <div className="actions">
          <button onClick={storeCred}>Store</button>
          <button className="ghost" onClick={checkCred}>Check exists</button>
          <button className="ghost" onClick={removeCred}>Remove</button>
        </div>
        {credStatus && <p className="status">{credStatus}</p>}
      </section>

      <section className="card">
        <h2>4 · HTTP POST (TLS, from C++)</h2>
        <p className="hint">Runs on a C++ worker thread (cpp-httplib + OpenSSL). Adds a keychain <code>Bearer</code> token if one exists for the host.</p>
        <label>URL</label>
        <input value={url} onChange={(e) => setUrl(e.target.value)} />
        <label>JSON body</label>
        <textarea rows={3} value={body} onChange={(e) => setBody(e.target.value)} />
        <div className="actions"><button onClick={doPost} disabled={httpBusy}>{httpBusy ? "Sending…" : "POST"}</button></div>
        {httpOut && <pre>{JSON.stringify(httpOut, null, 2)}</pre>}
      </section>

      <section className="card">
        <h2>5 · Printers</h2>
        <p className="hint">Discover (Winspool / CUPS), then print a test message — a text document that works with any printer, incl. Microsoft Print to PDF.</p>
        <div className="actions"><button className="ghost" onClick={discover}>Discover printers</button></div>
        <label>Printer</label>
        <select value={printer} onChange={(e) => setPrinter(e.target.value)}>
          {printers.length === 0 && <option value="">— none —</option>}
          {printers.map((p) => <option key={p.name} value={p.name}>{p.name}{p.isDefault ? " (default)" : ""}</option>)}
        </select>
        <label>Message</label>
        <input value={printText} onChange={(e) => setPrintText(e.target.value)} />
        <div className="actions"><button onClick={testPrint} disabled={!printer}>Print test message</button></div>
        {printStatus && <p className="status">{printStatus}</p>}
      </section>

      <section className="card">
        <h2>6 · Notes (SQLite)</h2>
        <p className="hint">Parameterized SQLite in the C++ backend, stored in the per-user app dir.
          <code>migrate</code> sets up the schema once; add/delete persist across restarts.</p>
        <div className="grid2">
          <input value={newNote} placeholder="Write a note…"
            onChange={(e) => setNewNote(e.target.value)}
            onKeyUp={(e) => e.key === "Enter" && addNote()} />
          <button style={{ flex: "0 0 auto" }} onClick={addNote}>Add</button>
        </div>
        <ul className="log">
          {notes.length === 0 && <li className="muted">no notes yet</li>}
          {notes.map((n) => (
            <li key={n.id} style={{ display: "flex", justifyContent: "space-between", gap: ".5rem" }}>
              <span>{n.body}</span>
              <a href="#" className="muted" onClick={(e) => { e.preventDefault(); deleteNote(n.id); }}>delete</a>
            </li>
          ))}
        </ul>
        {dbError && <p className="err">{dbError}</p>}
      </section>

      <section className="card">
        <h2>7 · Files (uploads)</h2>
        <p className="hint">Stored in the per-user app dir through the secure layer; names are
          sanitized (no path traversal). Pick a file to upload, then view or delete it.</p>
        <input type="file" onChange={pickFile} />
        <ul className="log">
          {fileList.length === 0 && <li className="muted">no files yet</li>}
          {fileList.map((f) => (
            <li key={f.name} style={{ display: "flex", justifyContent: "space-between", gap: ".5rem" }}>
              <span>{f.name} <span className="muted">({f.size} B)</span></span>
              <span>
                <a href="#" className="muted" onClick={(e) => { e.preventDefault(); viewFile(f.name); }}>view</a>{" · "}
                <a href="#" className="muted" onClick={(e) => { e.preventDefault(); removeFile(f.name); }}>delete</a>
              </span>
            </li>
          ))}
        </ul>
        {filePreview && <pre>{filePreview.name}:{"\n"}{filePreview.text}</pre>}
        {fileError && <p className="err">{fileError}</p>}
      </section>

      <section className="card">
        <h2>8 · Image upload (single)</h2>
        <p className="hint">Upload one image — it's stored through the secure file layer and
          shown below. Uploading another replaces (deletes) the previous one. The preview
          survives restarts.</p>
        <input type="file" accept="image/*" onChange={pickImage} />
        {imageUrl ? (
          <div className="imgbox">
            <img className="preview" src={imageUrl} alt={imageName} />
            <div className="imgmeta">
              <span className="muted">{imageName}</span>
              <button className="ghost" onClick={deleteImage}>Delete image</button>
            </div>
          </div>
        ) : (
          <p className="muted">no image uploaded yet</p>
        )}
        {imageError && <p className="err">{imageError}</p>}
      </section>

      <footer>Built with @mwguerra/hull · no Electron, no bundled browser</footer>
    </div>
  );
}
