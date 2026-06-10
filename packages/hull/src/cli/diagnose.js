import fs from "node:fs";
import { execFileSync } from "node:child_process";

// The host links these at load time (web view, keychain, printing, TLS). When one
// is missing the dynamic loader fails with a one-line stderr and exit code 127 —
// and only reports the FIRST missing library, so users fix them one by one. The
// ldd preflight below reports all of them at once, mapped to distro packages.
const LIB_PACKAGES = [
  { prefix: "libwebkitgtk-6.0", apt: "libwebkitgtk-6.0-4", dnf: "webkitgtk6.0", pacman: "webkitgtk-6.0" },
  { prefix: "libgtk-4",         apt: "libgtk-4-1",         dnf: "gtk4",         pacman: "gtk4" },
  { prefix: "libsecret-1",      apt: "libsecret-1-0",      dnf: "libsecret",    pacman: "libsecret" },
  { prefix: "libcups",          apt: "libcups2",           dnf: "cups-libs",    pacman: "libcups" },
  { prefix: "libavahi-client",  apt: "libavahi-client3",   dnf: "avahi-libs",   pacman: "avahi" },
  { prefix: "libssl",           apt: "libssl3",            dnf: "openssl-libs", pacman: "openssl" },
  { prefix: "libcrypto",        apt: "libssl3",            dnf: "openssl-libs", pacman: "openssl" },
];

export function packageManager() {
  if (process.platform !== "linux") return null;
  if (fs.existsSync("/usr/bin/apt-get") || fs.existsSync("/usr/bin/apt")) return "apt";
  if (fs.existsSync("/usr/bin/dnf")) return "dnf";
  if (fs.existsSync("/usr/bin/pacman")) return "pacman";
  return null;
}

// Sonames the binary needs but the system doesn't have (Linux only; [] elsewhere
// or when ldd itself is unavailable — never blocks on detection failure).
export function missingLibraries(binary) {
  if (process.platform !== "linux") return [];
  try {
    const out = execFileSync("ldd", [binary], {
      encoding: "utf8", stdio: ["ignore", "pipe", "pipe"],
    });
    return [...out.matchAll(/^\s*(\S+)\s+=>\s+not found/gm)].map((m) => m[1]);
  } catch {
    return [];
  }
}

// One install command covering every missing library (per the detected package
// manager), plus a list of anything we couldn't map.
export function installHint(missing) {
  const pm = packageManager() ?? "apt";
  const pkgs = new Set();
  const unknown = [];
  for (const so of missing) {
    const hit = LIB_PACKAGES.find((l) => so.startsWith(l.prefix));
    if (hit) pkgs.add(hit[pm] ?? hit.apt);
    else unknown.push(so);
  }
  const cmd = pm === "dnf" ? "sudo dnf install" : pm === "pacman" ? "sudo pacman -S" : "sudo apt install";
  let s = "";
  if (pkgs.size) s += `    ${cmd} ${[...pkgs].join(" ")}\n`;
  if (unknown.length) s += `    (also missing, package name unknown here: ${unknown.join(", ")})\n`;
  return s;
}

// Full, actionable error for a host that cannot start due to missing libraries.
export function missingLibsError(binary, missing) {
  return (
    `the native host needs system libraries that aren't installed:\n` +
    missing.map((m) => `    ${m}`).join("\n") + "\n" +
    `  install them with:\n` + installHint(missing) +
    `  then re-run. (Hull renders in the OS web view and uses the system keychain\n` +
    `  and printing — those runtimes come from these packages.)`
  );
}

// Friendly translation of spawn() errors; null when the raw error is already best.
export function explainSpawnError(err, binary) {
  if (!err) return null;
  if (err.code === "EACCES") return `the host binary isn't executable. Fix: chmod +x "${binary}"`;
  if (err.code === "ENOENT") return `the host binary is missing at ${binary}. Reinstall with: npm i -D @mwguerra/hull`;
  return null;
}

// One-line trailer for any non-zero host exit, pointing at the two tools that
// turn a silent failure into a diagnosis.
export function exitAdvice(cmd, code) {
  return `hull ${cmd}: the native host exited with code ${code}. ` +
    `Run "npx hull doctor" to check this machine, or re-run with --debug ` +
    `(verbose host log + web-view devtools).`;
}
