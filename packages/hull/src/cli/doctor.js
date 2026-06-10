import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { loadConfig } from "./config.js";
import { currentTarget, resolveHostFor } from "./host.js";
import { missingLibraries, installHint } from "./diagnose.js";

// hull doctor — check this machine for everything Hull needs to run, and print
// copy-pasteable fixes. Read-only except for one throwaway write probe in the
// app's own data dir; never prints settings, credentials, or file contents.
const ok   = (s) => console.log(`  ok    ${s}`);
const bad  = (s) => { console.log(`  FAIL  ${s}`); problems++; };
const warn = (s) => console.log(`  warn  ${s}`);
const info = (s) => console.log(`        ${s}`);
let problems = 0;

export async function doctor(cwd) {
  problems = 0;
  const key = currentTarget();
  console.log(`hull doctor — ${key} · node ${process.version} · ${os.type()} ${os.release()}\n`);

  // 1) project config
  let cfg = null;
  try {
    cfg = await loadConfig(cwd);
    ok(`config: appId=${cfg.appId} · title="${cfg.title}"${cfg.secure ? " · secure build" : ""}`);
  } catch (e) {
    bad(`config: ${e.message}`);
  }

  // 2) prebuilt host binary
  const found = await resolveHostFor(key);
  const binary = found && (cfg?.secure ? found.secureBinary : found.hostBinary);
  if (!binary) {
    bad(`prebuilt host for ${key} not found — reinstall: npm i -D @mwguerra/hull`);
  } else {
    ok(`host binary: ${binary}`);
    if (process.platform !== "win32") {
      try { fs.accessSync(binary, fs.constants.X_OK); ok("host binary is executable"); }
      catch { bad(`host binary not executable — fix: chmod +x "${binary}"`); }
    }
    if (process.platform === "linux") {
      const missing = missingLibraries(binary);
      if (missing.length) {
        bad(`missing system libraries: ${missing.join(", ")}`);
        info("install them with:");
        process.stdout.write(installHint(missing));
      } else {
        ok("system libraries: all of the host's shared libraries resolve (ldd)");
      }
    }
  }

  // 3) the OS web view runtime
  if (process.platform === "win32") {
    const dirs = [
      path.join(process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)", "Microsoft", "EdgeWebView", "Application"),
      path.join(process.env.ProgramFiles ?? "C:\\Program Files", "Microsoft", "EdgeWebView", "Application"),
    ];
    const wv = dirs.find((p) => fs.existsSync(p));
    if (wv) {
      const versions = fs.readdirSync(wv).filter((d) => /^\d+\./.test(d)).sort();
      ok(`WebView2 runtime: ${versions.at(-1) ?? "present"}`);
    } else {
      warn("WebView2 runtime not in the standard location — preinstalled on Windows 11; " +
           "on Windows 10 install Microsoft's Evergreen runtime");
    }
  } else if (process.platform === "darwin") {
    ok("WebKit ships with macOS — nothing to install");
  }

  // 4) the app's data dir is writable (settings/DB/files live there)
  if (cfg) {
    const base =
      process.platform === "win32" ? (process.env.LOCALAPPDATA ?? os.tmpdir())
      : process.platform === "darwin" ? path.join(os.homedir(), "Library", "Application Support")
      : (process.env.XDG_DATA_HOME ?? path.join(os.homedir(), ".local", "share"));
    const dir = path.join(base, cfg.appId);
    try {
      fs.mkdirSync(dir, { recursive: true });
      const probe = path.join(dir, ".hull-doctor");
      fs.writeFileSync(probe, "ok");
      fs.rmSync(probe);
      ok(`app data dir writable: ${dir}`);
    } catch (e) {
      bad(`app data dir not writable: ${dir} (${e.message})`);
    }
  }

  // 5) is there something to start?
  if (cfg) {
    const bundle = path.join(cwd, cfg.releaseDir, "development", key + (cfg.secure ? "-secure" : ""));
    if (fs.existsSync(path.join(bundle, "app.html")) ||
        (process.platform === "darwin" && fs.existsSync(bundle))) {
      ok(`development build present — "hull start" is ready`);
    } else {
      info(`no development build yet — run "hull build" before "hull start"`);
    }
  }

  console.log(problems
    ? `\n${problems} problem(s) found — fixes above.`
    : `\nAll checks passed. If a window still opens blank, re-run with --debug` +
      `\n(verbose host log + web-view devtools):  npx hull start --debug`);
  if (problems) process.exitCode = 1;
}
