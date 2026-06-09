import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// The bundled default app icon (the Hull logo), used when .hullrc sets no icon.
const DEFAULT_ICON = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), "../../assets/hull-logo.png");

// Package-level defaults. A project's .hullrc overrides these per key (the `window`
// object is merged deeply); with no config file, all defaults apply.
const DEFAULTS = {
  window: { width: 1100, height: 760 },
  secure: false,
  debug: false,
  outDir: "dist",
  releaseDir: "release",
};

function readJson(p) {
  try { return JSON.parse(fs.readFileSync(p, "utf8")); } catch { return null; }
}

// Project config: .hullrc (JSON) in the project root, preferred over the older
// hull.config.json. Returns {} when none is present (=> all package defaults).
function readProjectConfig(cwd) {
  for (const f of [".hullrc", ".hullrc.json", "hull.config.json"]) {
    const p = path.join(cwd, f);
    if (fs.existsSync(p)) return readJson(p) ?? {};
  }
  return {};
}

export async function loadConfig(cwd) {
  const pkg = readJson(path.join(cwd, "package.json")) ?? {};
  const bareName = (pkg.name ?? "hull-app").replace(/^@[^/]+\//, "");
  const file = readProjectConfig(cwd);
  const win = { ...DEFAULTS.window, ...(file.window ?? {}) };

  // Icon: .hullrc window.icon (or top-level icon), resolved against the project; else
  // the bundled Hull logo. Falls back to the default if the configured file is missing.
  const iconCfg = win.icon ?? file.icon;
  let icon = DEFAULT_ICON;
  if (iconCfg) {
    const p = path.resolve(cwd, iconCfg);
    icon = fs.existsSync(p) ? p : DEFAULT_ICON;
  }

  // Linux WebKitGTK sandbox: true = force on, false = force off, undefined = auto
  // (the host probes for unprivileged user namespaces and disables it only if needed).
  const linuxSandbox = file.linux?.sandbox;

  // Installer/store metadata. license: SPDX id; publisher: human/developer name.
  const license = file.license ?? pkg.license ?? null;
  const rawAuthor = file.author ?? pkg.author ?? null;
  const publisher = !rawAuthor ? null
    : typeof rawAuthor === "string" ? rawAuthor.replace(/\s*[<(].*$/, "").trim()
    : (rawAuthor.name ?? null);
  const description = file.description ?? pkg.description ?? null;

  return {
    appId: file.appId ?? `com.hull.${bareName}`,
    title: win.title ?? file.title ?? pkg.productName ?? bareName,
    width: Number(win.width),
    height: Number(win.height),
    icon,
    secure: Boolean(file.secure ?? DEFAULTS.secure),
    debug: Boolean(file.debug ?? DEFAULTS.debug),
    linuxSandbox: typeof linuxSandbox === "boolean" ? linuxSandbox : undefined,
    license,
    publisher,
    description,
    outDir: file.outDir ?? DEFAULTS.outDir,
    releaseDir: file.releaseDir ?? DEFAULTS.releaseDir,
  };
}

// Environment additions for the spawned host that control the Linux WebKitGTK sandbox.
//   force off (cfg.linuxSandbox === false, or --no-sandbox) -> disable it up front
//   force on  (cfg.linuxSandbox === true)                   -> tell the host to keep it
//   otherwise                                               -> nothing (host auto-detects)
export function hostEnv(cfg, { noSandbox = false } = {}) {
  const env = {};
  if (noSandbox || cfg.linuxSandbox === false) {
    env.WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS = "1";
  } else if (cfg.linuxSandbox === true) {
    env.HULL_FORCE_SANDBOX = "1";
  }
  return env;
}

// Common host flags derived from config.
export function hostArgs(cfg) {
  const args = [
    "--title", cfg.title,
    "--app-id", cfg.appId,
    "--width", String(cfg.width),
    "--height", String(cfg.height),
  ];
  if (cfg.icon) args.push("--icon", cfg.icon);
  if (cfg.debug) args.push("--debug");
  return args;
}
