import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { loadConfig, hostArgs, hostEnv } from "./config.js";
import { resolveHost, currentTarget } from "./host.js";
import { parseVersion, sanitize } from "./release.js";
import { createTimer } from "./timing.js";

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

  // macOS: the build produced a .app — launch it via `open` so LaunchServices shows the
  // bundle icon (Dock/Finder). `-W` waits until the app quits.
  if (process.platform === "darwin") {
    const appPath = path.join(dir, `${sanitize(cfg.title)}.app`);
    if (!fs.existsSync(appPath)) throw missing();
    timer.total("hull start");
    const child = spawn("open", ["-W", appPath], { stdio: "inherit" });
    child.on("exit", (code) => process.exit(code ?? 0));
    return;
  }

  const appHtml = path.join(dir, "app.html");
  if (!fs.existsSync(appHtml)) throw missing();

  const { binary } = await resolveHost({ secure: cfg.secure });
  timer.step("host resolved");
  timer.total("hull start");
  const env = { ...process.env, ...hostEnv(cfg, { noSandbox: args.includes("--no-sandbox") }) };
  const child = spawn(binary, ["--app", appHtml, ...hostArgs(cfg)], { stdio: "inherit", env });
  child.on("exit", (code) => process.exit(code ?? 0));
}
