import fs from "node:fs";
import path from "node:path";
import { loadConfig } from "./config.js";
import { loadVite } from "./vite.js";
import { resolveHostFor, currentTarget, KNOWN_TARGETS, binaryName } from "./host.js";
import {
  parseVersion, sanitize, copyHostFiles, writeLauncher, makeArchive, defaultFormat,
  writeMacApp, makeAppArchive,
} from "./release.js";
import { createTimer } from "./timing.js";

// Usage:
//   hull build [vX.Y.Z] [--platform <key|all>] [--format zip|tar.gz]
// No version  -> release/development/...   ;  vX.Y.Z -> release/vX.Y.Z/...
function parseArgs(args) {
  let version = null, platform = null, format = null;
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === "--platform") platform = args[++i];
    else if (a === "--format") format = args[++i];
    else if (!a.startsWith("-")) version = a;
  }
  return { ...parseVersion(version), platform, format };
}

export async function build(cwd, args, { verbose } = {}) {
  const timer = createTimer(verbose);
  const { label, platform, format } = parseArgs(args);
  const cfg = await loadConfig(cwd);
  timer.step("config loaded");

  // Which platforms to package?
  let targetKeys;
  if (!platform) targetKeys = [currentTarget()];
  else if (platform === "all") targetKeys = KNOWN_TARGETS;
  else targetKeys = [platform];

  // Keep only targets whose prebuilt host binary (for the chosen flavor) is present.
  const flavor = cfg.secure ? "secure " : "";
  const hosts = [];
  for (const key of targetKeys) {
    const h = await resolveHostFor(key);
    const binPath = h && (cfg.secure ? h.secureBinary : h.hostBinary);
    if (h && binPath) hosts.push(h);
    else console.warn(`hull build: skipping ${key} (no ${flavor}host binary built)`);
  }
  if (hosts.length === 0) {
    throw new Error(
      `no ${flavor}host binary available for: ${targetKeys.join(", ")}.\n` +
      `  Build it (npm run build:host${cfg.secure ? ":secure" : ""}), or run on the matching OS / CI.`
    );
  }
  timer.step(`resolved ${hosts.length} host(s)`);

  // Build the single-file UI ONCE, shared by every platform bundle.
  const vite = await loadVite(cwd);
  const { viteSingleFile } = await import("vite-plugin-singlefile");
  timer.step("vite loaded");
  console.log("hull build: bundling UI into a single file…");
  await vite.build({
    root: cwd,
    plugins: [viteSingleFile()],
    build: { outDir: cfg.outDir, cssCodeSplit: false, target: "esnext", emptyOutDir: true },
    logLevel: "warn",
  });
  timer.step("vite single-file build");
  const builtHtml = path.join(cwd, cfg.outDir, "index.html");
  if (!fs.existsSync(builtHtml)) {
    throw new Error(`expected ${path.relative(cwd, builtHtml)} after build — is this a Vite app?`);
  }

  // Assemble + archive one bundle per target.
  const versionRoot = path.join(cwd, cfg.releaseDir, label);
  fs.mkdirSync(versionRoot, { recursive: true });
  const results = [];

  for (const h of hosts) {
    const binName = binaryName(h.key, cfg.secure);
    const bundleDir = path.join(versionRoot, h.key + (cfg.secure ? "-secure" : ""));
    fs.rmSync(bundleDir, { recursive: true, force: true });
    const rootName = `${sanitize(cfg.title)}-${label}-${h.key}${cfg.secure ? "-secure" : ""}`;

    // macOS: produce a real .app bundle (Dock/Finder icon, double-click) + tar.gz.
    if (h.key.startsWith("darwin")) {
      fs.mkdirSync(bundleDir, { recursive: true });
      const { appName } = writeMacApp(bundleDir, cfg, h.hostDir, binName, builtHtml, cfg.icon);
      const archivePath = path.join(versionRoot, `${rootName}.tar.gz`);
      const bytes = await makeAppArchive(bundleDir, archivePath, rootName);
      results.push({ key: h.key, archivePath, bytes, app: appName });
      timer.step(`packaged ${h.key} (.app)`);
      continue;
    }

    copyHostFiles(h.hostDir, bundleDir, binName);
    fs.copyFileSync(builtHtml, path.join(bundleDir, "app.html"));
    // Bundle the window icon so the distributed app shows it (no node_modules at runtime).
    let iconName = null;
    if (cfg.icon && fs.existsSync(cfg.icon)) {
      iconName = "icon" + path.extname(cfg.icon);
      fs.copyFileSync(cfg.icon, path.join(bundleDir, iconName));
    }
    const launcher = writeLauncher(bundleDir, h.key, cfg, binName, iconName);

    const fmt = format ?? defaultFormat(h.key);
    const ext = fmt === "zip" ? "zip" : "tar.gz";
    const archivePath = path.join(versionRoot, `${rootName}.${ext}`);
    const bytes = await makeArchive(bundleDir, archivePath, fmt, rootName, h.key, launcher.name, binName);
    results.push({ key: h.key, archivePath, bytes });
    timer.step(`packaged ${h.key}`);
  }

  // Summary.
  const rel = (p) => path.relative(cwd, p).replace(/\\/g, "/");
  console.log(`\nhull build: ${label} -> ${rel(versionRoot)}/`);
  for (const r of results) {
    console.log(`  ${r.key.padEnd(14)} ${rel(r.archivePath)}  (${(r.bytes / 1024).toFixed(0)} KB)`);
  }
  if (results.some((r) => r.key === currentTarget())) {
    console.log(`  run it locally with "hull start${label === "development" ? "" : " " + label}"`);
  }
  timer.total("\nhull build");
}
