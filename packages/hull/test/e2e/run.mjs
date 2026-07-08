// End-to-end test for native fullscreen + the link policy, against the REAL host.
//
//   npm -w @mwguerra/hull run test:e2e   (needs a built host: npm run build:host)
//
// How it works: launches `hull-host --app fixture.html --inspect --inspect-port N`
// with HULL_SHELL_DRYRUN=1 (the shell layer prints instead of opening browsers /
// spawning windows). The fixture clicks its own links; this harness watches the
// __trace SSE stream to assert which bindings fired, then drives the fullscreen
// bindings over the trace server's HTTP invoke endpoint. A native window opens
// briefly and enters/leaves fullscreen — that's the test observing real behavior.
import http from "node:http";
import net from "node:net";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { resolveHost } from "../../src/cli/host.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE = path.join(here, "fixture.html");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let failures = 0;
function check(cond, what) {
  console.log(`${cond ? "ok  " : "FAIL"} ${what}`);
  if (!cond) failures++;
}

function freePort() {
  return new Promise((res, rej) => {
    const s = net.createServer();
    s.on("error", rej);
    s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); });
  });
}

async function waitForHealth(base) {
  for (let i = 0; i < 100; i++) {
    try { const r = await fetch(`${base}/health`); if (r.ok) return true; } catch { /* not up yet */ }
    await sleep(100);
  }
  return false;
}

// Collect __trace frames from the SSE stream into `traces` as they arrive.
function collectTraces(base, traces, signal) {
  return (async () => {
    const res = await fetch(`${base}/bridge/events`, { signal });
    let buf = "";
    for await (const chunk of res.body) {
      buf += Buffer.from(chunk).toString("utf8");
      let i;
      while ((i = buf.indexOf("\n\n")) >= 0) {
        const frame = buf.slice(0, i); buf = buf.slice(i + 2);
        const line = frame.split("\n").find((l) => l.startsWith("data: "));
        if (!line) continue;
        try {
          const msg = JSON.parse(line.slice(6));
          if (msg.event === "__trace") traces.push(msg.payload);
        } catch { /* keepalive/partial */ }
      }
    }
  })().catch(() => { /* stream closes when the host exits */ });
}

const invoke = async (base, name, args = []) => {
  const r = await fetch(`${base}/bridge/invoke`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name, args }),
  });
  return r.json();
};

async function waitFor(predicate, timeoutMs, what) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (predicate()) return true;
    await sleep(100);
  }
  console.log(`FAIL timed out waiting for: ${what}`);
  failures++;
  return false;
}

// Foreign-origin probe server (fixed port — the fixture hardcodes it). The fixture
// PROGRAMMATICALLY navigates the bridged window here; the origin guard must have
// stripped every binding, and probe.html reports what it sees back to /report.
const PROBE_PORT = 38217;
let probeReport = null;
const probeServer = http.createServer((req, res) => {
  const url = new URL(req.url, `http://127.0.0.1:${PROBE_PORT}`);
  if (url.pathname === "/probe.html") {
    res.setHeader("Content-Type", "text/html; charset=utf-8");
    res.end(`<!doctype html><title>probe</title><script>
      fetch("/report?" + new URLSearchParams({
        ping: typeof window.ping,
        dbExec: typeof window.dbExec,
        saveCredential: typeof window.saveCredential,
        openExternal: typeof window.openExternal,
        bridgeEmit: typeof window.__bridgeEmit,
        webviewRpc: typeof window.__webview__,
      }));
    </script>`);
    return;
  }
  if (url.pathname === "/report") {
    probeReport = Object.fromEntries(url.searchParams);
    res.end("ok");
    return;
  }
  res.statusCode = 404; res.end();
});
await new Promise((res, rej) => {
  probeServer.on("error", rej);
  probeServer.listen(PROBE_PORT, "127.0.0.1", res);
}).catch((e) => { console.error(`FAIL probe server on :${PROBE_PORT}: ${e.message}`); process.exit(1); });

// ---- main ----
const { binary } = await resolveHost({ secure: false }); // throws with build advice if absent
const port = await freePort();
const base = `http://127.0.0.1:${port}`;

const host = spawn(
  binary,
  ["--app", FIXTURE, "--inspect", "--inspect-port", String(port),
   "--title", "hull-e2e", "--app-id", "com.hull.e2e", "--width", "480", "--height", "360"],
  { stdio: ["ignore", "inherit", "inherit"], env: { ...process.env, HULL_SHELL_DRYRUN: "1" } },
);
host.on("error", (e) => { console.error(`FAIL host spawn: ${e.message}`); process.exit(1); });

const aborter = new AbortController();
const traces = [];
try {
  check(await waitForHealth(base), "trace server is up");
  collectTraces(base, traces, aborter.signal);

  // --- link policy (fixture drives itself; we watch the trace) ---
  const doneTrace = () =>
    traces.find((t) => t.type === "call" && t.name === "ping" &&
                       typeof t.args?.[0] === "string" && t.args[0].includes("links-done"));
  await waitFor(doneTrace, 20000, "fixture finished its link clicks");

  const calls = (name) => traces.filter((t) => t.type === "call" && t.name === name);
  const externalUrls = calls("openExternal").map((t) => t.args?.[0]);
  const windowUrls = calls("openWindow").map((t) => t.args?.[0]);

  check(externalUrls.includes("https://example.com/external"), "plain external link -> openExternal");
  check(externalUrls.includes("https://example.com/blank"), "target=_blank link -> openExternal");
  check(externalUrls.includes("https://example.com/nested"), "click on element nested in <a> -> openExternal");
  check(externalUrls.includes("https://example.com/svg"), "SVG <a> link -> openExternal");
  check(externalUrls.includes("https://example.com/area"), "<area> link -> openExternal");
  check(externalUrls.includes("https://example.com/middle"), "middle-click (auxclick) -> openExternal");
  check(externalUrls.includes("mailto:test@example.com"), "mailto link -> openExternal");
  check(externalUrls.includes("https://example.com/scripted"), "window.open(external) -> openExternal");
  check(!externalUrls.some((u) => String(u).startsWith("javascript:")),
        "window.open(javascript:) never reaches openExternal");
  check(externalUrls.length === 8, `exactly 8 openExternal calls (got ${externalUrls.length})`);
  check(windowUrls.length === 1 && windowUrls[0] === "https://example.com/child",
        "data-hull-window link -> openWindow (and nothing else)");

  const okReplies = traces.filter((t) => t.type === "reply" &&
      (t.name === "openExternal" || t.name === "openWindow") && t.ok === true);
  check(okReplies.length === 9, `all 9 open* replies ok (got ${okReplies.length})`);

  const done = doneTrace();
  const payload = done ? JSON.parse(done.args[0]) : {};
  check(String(payload.href).startsWith("file://") && String(payload.href).includes("fixture.html"),
        "page did NOT navigate away from the app");
  check(payload.hash === "#local", "in-page hash link kept its default behavior");

  // --- URL policy over the bridge (fail closed) ---
  const bad = await invoke(base, "openExternal", ["file:///etc/passwd"]);
  check(bad.ok === false, "openExternal rejects file:// URLs");
  const badWin = await invoke(base, "openWindow", ["mailto:x@y.z"]);
  check(badWin.ok === false, "openWindow rejects non-web URLs");

  // --- origin guard: the fixture now navigates itself (location.href — the one
  // thing click interception can't stop) to the foreign probe page, which
  // reports what it can see. Every binding must be stripped there.
  await waitFor(() => probeReport !== null, 15000, "foreign probe page reported in");
  if (probeReport) {
    const leaked = Object.entries(probeReport).filter(([, type]) => type !== "undefined");
    check(leaked.length === 0,
          `foreign origin sees NO bridge (leaked: ${leaked.map(([k, v]) => `${k}=${v}`).join(", ") || "none"})`);
  }

  // --- fullscreen (real native window; macOS animates, hence the sleeps) ---
  const fs0 = await invoke(base, "isFullscreen");
  check(fs0.ok === true && fs0.fullscreen === false, "starts windowed");
  const fsOn = await invoke(base, "setFullscreen", [true]);
  check(fsOn.ok === true && fsOn.fullscreen === true, "setFullscreen(true) acknowledged");
  await sleep(2000);
  const fs1 = await invoke(base, "isFullscreen");
  check(fs1.ok === true && fs1.fullscreen === true, "window IS fullscreen after transition");
  const fsOff = await invoke(base, "setFullscreen", [false]);
  check(fsOff.ok === true, "setFullscreen(false) acknowledged");
  await sleep(2000);
  const fs2 = await invoke(base, "isFullscreen");
  check(fs2.ok === true && fs2.fullscreen === false, "window left fullscreen");
} finally {
  aborter.abort();
  probeServer.close();
  host.kill("SIGTERM");
  await sleep(300);
  try { host.kill("SIGKILL"); } catch { /* already gone */ }
}

if (failures) { console.error(`\n${failures} FAILURE(S)`); process.exit(1); }
console.log("\nall e2e tests passed");
process.exit(0);
