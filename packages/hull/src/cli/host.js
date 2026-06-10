import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Known target keys (os-arch), matching the platform package names.
export const KNOWN_TARGETS = [
  "win32-x64",
  "darwin-arm64",
  "linux-x64",
];

export const currentTarget = () => `${process.platform}-${process.arch}`;

const here = path.dirname(fileURLToPath(import.meta.url));

// Load a platform package's entry. We try a normal import first (works when the
// package is installed for end users via the published optionalDependencies), then
// fall back to the sibling path. The sibling path is identical in both layouts:
//   monorepo:   packages/hull/src/cli  ->  ../../../hull-<key>
//   installed:  @mwguerra/hull/src/cli ->  ../../../hull-<key>  (same @mwguerra scope)
async function loadPlatform(key) {
  try {
    return await import(`@mwguerra/hull-${key}`);
  } catch {
    const rel = path.resolve(here, `../../../hull-${key}/index.js`);
    if (fs.existsSync(rel)) return import(pathToFileURL(rel).href);
    return null;
  }
}

// The on-disk binary name for a platform + flavor.
export function binaryName(key, secure) {
  const base = secure ? "hull-host-secure" : "hull-host";
  return key.startsWith("win32-") ? `${base}.exe` : base;
}

// npm tarballs packed in CI can lose the executable bit (the actions artifact
// zip round-trip strips file modes), so a freshly installed host may not be
// runnable on macOS/Linux. Repair it here so every caller gets a spawnable path.
function ensureExecutable(bin) {
  if (!bin || process.platform === "win32") return bin;
  try { fs.accessSync(bin, fs.constants.X_OK); }
  catch { try { fs.chmodSync(bin, 0o755); } catch { /* surfaces at spawn */ } }
  return bin;
}

// Resolve a specific platform's prebuilt host. Returns null if neither the default
// nor secure binary has been built yet (so callers can skip the platform).
export async function resolveHostFor(key) {
  const mod = await loadPlatform(key);
  if (!mod || !mod.hostDir) return null;
  const hostBinary = mod.hostBinary && fs.existsSync(mod.hostBinary)
    ? ensureExecutable(mod.hostBinary) : null;
  const secureBinary = mod.secureBinary && fs.existsSync(mod.secureBinary)
    ? ensureExecutable(mod.secureBinary) : null;
  if (!hostBinary && !secureBinary) return null;
  return { key, pkg: `@mwguerra/hull-${key}`, hostDir: mod.hostDir, hostBinary, secureBinary };
}

// Resolve the runnable binary for the CURRENT platform and the requested flavor,
// throwing a helpful error if it isn't built.
export async function resolveHost({ secure = false } = {}) {
  const key = currentTarget();
  const found = await resolveHostFor(key);
  const binary = found && (secure ? found.secureBinary : found.hostBinary);
  if (binary) return { ...found, binary, secure };
  const flavor = secure ? "secure host (-DHULL_CRYPTO=ON)" : "host";
  throw new Error(
    `no prebuilt Hull ${flavor} for "${key}".\n` +
    `  Build it with "npm run build:host${secure ? ":secure" : ""}", install the published\n` +
    `  @mwguerra/hull-${key}, or run "hull eject" to build from source.`
  );
}
