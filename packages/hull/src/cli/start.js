import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { loadConfig, hostArgs, hostEnv } from "./config.js";
import { resolveHost, currentTarget, binaryName } from "./host.js";
import { parseVersion, sanitize } from "./release.js";
import { createTimer } from "./timing.js";
import { missingLibraries, missingLibsError, explainSpawnError, exitAdvice } from "./diagnose.js";

// Run a packaged app:  hull start [vX.Y.Z]   (defaults to the development build)
export async function start(cwd, args, { verbose } = {}) {
  const timer = createTimer(verbose);
  const version = args.find((a) => !a.startsWith("-")) ?? null;
  const { label } = parseVersion(version);
  const cfg = await loadConfig(cwd);
  timer.step("config loaded");

  // The packaged bundle dir carries a "-secure" suffix when secure.
  const bundleDir = currentTarget() + (cfg.secure ? "-secure" : "");
  const dir = path.join(cwd, cfg.releaseDir, label, bundleDir);
  const missing = () => {
    const v = label === "development" ? "" : " " + label;
    return new Error(`no build at ${path.relative(cwd, dir)}. Run "hull build${v}" first.`);
  };

  const debug = args.includes("--debug");

  // macOS: the build produced a .app — launch it via `open` so LaunchServices shows the
  // bundle icon (Dock/Finder). `-W` waits until the app quits. With --debug we spawn the
  // bundled binary directly instead: the .app launcher has baked args, so neither the
  // flag nor the host's stderr log can reach the terminal through `open`.
  if (process.platform === "darwin") {
    const appPath = path.join(dir, `${sanitize(cfg.title)}.app`);
    if (!fs.existsSync(appPath)) throw missing();
    timer.total("hull start");
    if (debug) {
      const inner = path.join(appPath, "Contents", "MacOS", binaryName(currentTarget(), cfg.secure));
      const appHtml = path.join(appPath, "Contents", "Resources", "app.html");
      const child = spawn(inner, ["--app", appHtml, ...hostArgs(cfg), "--debug"], { stdio: "inherit" });
      child.on("error", (err) => { console.error(`hull start: ${explainSpawnError(err, inner) ?? err.message}`); process.exit(1); });
      child.on("exit", (code) => { if (code) console.error(exitAdvice("start", code)); process.exit(code ?? 0); });
      return;
    }
    const child = spawn("open", ["-W", appPath], { stdio: "inherit" });
    child.on("exit", (code) => process.exit(code ?? 0));
    return;
  }

  const appHtml = path.join(dir, "app.html");
  if (!fs.existsSync(appHtml)) throw missing();

  const { binary } = await resolveHost({ secure: cfg.secure });
  timer.step("host resolved");

  // Linux preflight: the dynamic loader reports only the FIRST missing library,
  // one failure at a time — ldd lets us list all of them with one install command.
  if (process.platform === "linux") {
    const libs = missingLibraries(binary);
    if (libs.length) throw new Error(missingLibsError(binary, libs));
  }

  timer.total("hull start");
  const env = { ...process.env, ...hostEnv(cfg, { noSandbox: args.includes("--no-sandbox") }) };
  const child = spawn(binary, ["--app", appHtml, ...hostArgs(cfg), ...(debug && !cfg.debug ? ["--debug"] : [])],
                      { stdio: "inherit", env });
  child.on("error", (err) => { console.error(`hull start: ${explainSpawnError(err, binary) ?? err.message}`); process.exit(1); });
  child.on("exit", (code) => { if (code) console.error(exitAdvice("start", code)); process.exit(code ?? 0); });
}
