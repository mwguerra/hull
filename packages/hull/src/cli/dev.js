import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { loadConfig, hostArgs, hostEnv } from "./config.js";
import { resolveHost } from "./host.js";
import { loadVite } from "./vite.js";
import { createTimer } from "./timing.js";
import { missingLibraries, missingLibsError, explainSpawnError, exitAdvice } from "./diagnose.js";

// Linux: list every missing system library up front (the loader would fail on
// just the first one). Throws with a single copy-pasteable install command.
function preflight(binary) {
  if (process.platform !== "linux") return;
  const libs = missingLibraries(binary);
  if (libs.length) throw new Error(missingLibsError(binary, libs));
}

const here = path.dirname(fileURLToPath(import.meta.url));
const INSPECTOR_HTML = path.resolve(here, "../../devtools/dist/index.html"); // built single file

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function freePort() {
  return new Promise((res, rej) => {
    const s = net.createServer();
    s.on("error", rej);
    s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); });
  });
}
function openUrl(url) {
  const [cmd, args] =
    process.platform === "win32" ? ["cmd", ["/c", "start", "", url]]
    : process.platform === "darwin" ? ["open", [url]]
    : ["xdg-open", [url]];
  try { spawn(cmd, args, { stdio: "ignore", detached: true }).unref(); } catch { /* ignore */ }
}
async function waitForHealth(url) {
  for (let i = 0; i < 60; i++) {
    try { const r = await fetch(`${url}/health`); if (r.ok) return true; } catch { /* not up yet */ }
    await sleep(100);
  }
  return false;
}

// Make sure the dev server is actually serving the app before we open the native
// window. server.listen() resolves when the port is bound, but on a cold start (or
// after Vite re-optimizes deps) the first load can come back incomplete, and WebKitGTK
// shows a blank page without retrying -> intermittent white screen in `hull dev`. So we
// fetch the index, then warm the entry module (runs Vite's transform + dep optimize) so
// the page the web view loads is already fully ready.
async function waitForApp(server, url) {
  let html = null;
  for (let i = 0; i < 120 && html === null; i++) {            // ~6s
    try { const r = await fetch(url); if (r.ok) html = await r.text(); } catch { /* not up */ }
    if (html === null) await sleep(50);
  }
  if (!html) return;
  const m = html.match(/<script[^>]*type=["']module["'][^>]*src=["']([^"']+)["']/i);
  if (!m) return;
  try { await server.warmupRequest?.(m[1]); } catch { /* best effort */ }
  try { await fetch(new URL(m[1], url).href); } catch { /* best effort */ }
}

// Vite plugin (browser mode only): inject the bridge URL into every served page and
// serve the inspector at /__hull/devtools. Never part of `hull build`.
function devBrowserPlugin(bridgeUrl) {
  const inject = (html) =>
    html.replace(/<head>/i, `<head><script>window.__HULL_BRIDGE__=${JSON.stringify(bridgeUrl)};</script>`);
  return {
    name: "hull:dev-browser",
    transformIndexHtml(html) { return inject(html); },
    configureServer(server) {
      server.middlewares.use("/__hull/devtools", (_req, res) => {
        let html = "<!doctype html><p>Inspector not built. Run <code>npm run build:devtools</code>.</p>";
        try { html = inject(fs.readFileSync(INSPECTOR_HTML, "utf8")); } catch { /* fall through */ }
        res.setHeader("Content-Type", "text/html; charset=utf-8");
        res.end(html);
      });
    },
  };
}

export async function dev(cwd, args, { verbose } = {}) {
  const timer = createTimer(verbose);
  const browser = args.includes("--browser");
  const cfg = await loadConfig(cwd);
  timer.step("config loaded");
  const vite = await loadVite(cwd);
  timer.step("vite loaded");

  // ---- Browser mode: UI in your browser (full HMR), bridge over HTTP/SSE ----
  if (browser) {
    const port = await freePort();
    const bridgeUrl = `http://127.0.0.1:${port}`;
    const server = await vite.createServer({
      root: cwd,
      server: { open: false },
      plugins: [devBrowserPlugin(bridgeUrl)],
    });
    await server.listen();
    const appUrl = server.resolvedUrls?.local?.[0] ?? `http://localhost:5173/`;
    timer.step("vite dev server");

    const { binary } = await resolveHost({ secure: cfg.secure });
    preflight(binary);
    const host = spawn(binary, ["--serve", String(port), "--inspect", "--app-id", cfg.appId],
                       { stdio: "inherit" });
    host.on("error", (err) => { console.error(`hull dev: ${explainSpawnError(err, binary) ?? err.message}`); process.exit(1); });
    await waitForHealth(bridgeUrl);
    timer.step("host bridge server");

    openUrl(appUrl);
    openUrl(`${appUrl}__hull/devtools`);
    console.log(`hull dev --browser${cfg.secure ? " (secure host)" : ""}:`);
    console.log(`  app:       ${appUrl}`);
    console.log(`  inspector: ${appUrl}__hull/devtools`);
    console.log(`  bridge:    ${bridgeUrl}  (edit UI freely — just reload, no recompile)`);
    timer.total("hull dev --browser ready");

    const shutdown = async () => { try { host.kill(); } catch {} try { await server.close(); } catch {} process.exit(0); };
    host.on("exit", shutdown);
    process.on("SIGINT", shutdown);
    process.on("SIGTERM", shutdown);
    return;
  }

  // ---- Native window mode: Vite dev server rendered in the host web view ----
  // The app runs in the native window; the companion inspector opens as a browser tab
  // fed by the host's trace server (same inspector UI as browser mode).
  const inspectPort = await freePort();
  const traceUrl = `http://127.0.0.1:${inspectPort}`;
  const server = await vite.createServer({
    root: cwd,
    server: { open: false },
    plugins: [devBrowserPlugin(traceUrl)], // serves the inspector at /__hull/devtools
  });
  await server.listen();
  const url =
    server.resolvedUrls?.local?.[0] ??
    `http://localhost:${server.config.server.port ?? 5173}/`;
  timer.step("vite dev server");

  console.log(`hull dev: serving ${url}${cfg.secure ? " (secure host)" : ""}`);

  // Don't open the window until the app is actually served (avoids a cold-start blank).
  await waitForApp(server, url);
  timer.step("dev server ready");

  const { binary } = await resolveHost({ secure: cfg.secure });
  preflight(binary);
  // --inspect enables the trace; --inspect-port runs the trace server for the inspector.
  const debug = args.includes("--debug") && !cfg.debug;
  const env = { ...process.env, ...hostEnv(cfg, { noSandbox: args.includes("--no-sandbox") }) };
  const child = spawn(
    binary,
    ["--url", url, "--inspect", "--inspect-port", String(inspectPort), ...hostArgs(cfg),
     ...(debug ? ["--debug"] : [])],
    { stdio: "inherit", env });
  child.on("error", (err) => { console.error(`hull dev: ${explainSpawnError(err, binary) ?? err.message}`); process.exit(1); });
  timer.step("native window launched");

  if (await waitForHealth(traceUrl)) {
    openUrl(`${url}__hull/devtools`);
    console.log(`  inspector: ${url}__hull/devtools`);
  }
  timer.total("hull dev ready");

  const shutdown = async () => { try { await server.close(); } catch {} process.exit(0); };
  child.on("exit", (code) => { if (code) console.error(exitAdvice("dev", code)); shutdown(); });
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}
