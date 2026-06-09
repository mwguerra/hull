// Build the native Hull host and stage it into the matching platform package's bin.
//
//   node scripts/build-host.mjs                 # current platform, native
//   node scripts/build-host.mjs --target linux-x64   # one target (native or via Docker)
//   node scripts/build-host.mjs --all           # everything buildable on THIS machine
//   node scripts/build-host.mjs --secure        # crypto build (SQLCipher + AES at rest)
//
// Cross-platform reality: a host must be built on its own OS. The one exception is
// Linux, which can be built from any OS via Docker. macOS and Windows hosts require
// their own OS (or CI). Honors env: VCPKG_ROOT, GIT_EXECUTABLE, OPENSSL_ROOT_DIR.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const hostDir = path.join(root, "packages", "hull", "host");
const KNOWN = ["win32-x64", "darwin-arm64", "linux-x64"];

// ---- args ----
const args = process.argv.slice(2);
const all = args.includes("--all");
const secure = args.includes("--secure");
let target = null;
for (let i = 0; i < args.length; i++) if (args[i] === "--target") target = args[++i];

const currentArch = process.arch;
const currentKey = `${process.platform}-${currentArch}`;

function normalize(token) {
  if (!token) return currentKey;
  if (token.includes("-")) return token;            // already os-arch
  if (token === "win32") return `win32-${currentArch}`;
  if (token === "darwin") return `darwin-${currentArch}`;
  if (token === "linux") return "linux-x64";
  return token;
}

// ---- tool discovery ----
function which(bin, fallbacks = []) {
  for (const c of [bin, ...fallbacks]) {
    try { execFileSync(c, ["--version"], { stdio: "ignore" }); return c; }
    catch { /* next */ }
  }
  return null;
}
const cmakeBin = () => {
  const c = which("cmake", process.platform === "win32" ? ["C:/Program Files/CMake/bin/cmake.exe"] : []);
  if (!c) throw new Error("cmake not found (install it or add it to PATH)");
  return c;
};
const dockerAvailable = () => {
  try { execFileSync("docker", ["info"], { stdio: "ignore" }); return true; }
  catch { return false; }
};

// Prefer an explicit GIT_EXECUTABLE; otherwise, on Windows, fall back to a real
// Git-for-Windows binary. Some setups put a git.cmd shim on PATH that pollutes
// stdout and breaks CMake FetchContent ("ambiguous argument 'HEAD0'").
function gitExecutable() {
  if (process.env.GIT_EXECUTABLE) return process.env.GIT_EXECUTABLE;
  if (process.platform === "win32") {
    for (const p of [
      "C:/Program Files/Git/cmd/git.exe",
      "C:/Program Files/Git/bin/git.exe",
      "C:/Program Files (x86)/Git/cmd/git.exe",
    ]) {
      if (fs.existsSync(p)) return p;
    }
  }
  return null; // use PATH default
}

function stageDir(key) {
  const d = path.join(root, "packages", `hull-${key}`, "bin");
  fs.mkdirSync(d, { recursive: true });
  return d;
}

// ---- native build (current OS only) ----
function buildNative(key, secure) {
  const cmake = cmakeBin();
  const buildDir = path.join(hostDir, secure ? "build-secure" : "build");
  const vcpkgRoot = process.env.VCPKG_ROOT || path.join(os.homedir(), "vcpkg");
  const toolchain = path.join(vcpkgRoot, "scripts", "buildsystems", "vcpkg.cmake");

  const configure = ["-S", hostDir, "-B", buildDir];
  if (process.platform === "win32") {
    // Let CMake pick the installed Visual Studio generator (VS 2022 / 2026 / …) instead
    // of pinning one — keeps working as GitHub rolls runners forward (windows-2025-vs2026).
    configure.push("-A", "x64", "-DVCPKG_TARGET_TRIPLET=x64-windows");
  } else {
    configure.push("-DCMAKE_BUILD_TYPE=Release");
  }
  if (secure) configure.push("-DHULL_CRYPTO=ON");
  if (fs.existsSync(toolchain)) configure.push(`-DCMAKE_TOOLCHAIN_FILE=${toolchain}`);
  const gitExe = gitExecutable();
  if (gitExe) configure.push(`-DGIT_EXECUTABLE=${gitExe}`);
  // macOS: Homebrew OpenSSL isn't on the default search path.
  if (process.env.OPENSSL_ROOT_DIR) configure.push(`-DOPENSSL_ROOT_DIR=${process.env.OPENSSL_ROOT_DIR}`);

  execFileSync(cmake, configure, { stdio: "inherit" });
  execFileSync(cmake, ["--build", buildDir, "--config", "Release"], { stdio: "inherit" });

  const srcDirs = [path.join(buildDir, "bin", "Release"), path.join(buildDir, "bin")];
  const srcBin = srcDirs.find((d) => fs.existsSync(d) && fs.readdirSync(d).some((f) => f.startsWith("hull-host")));
  if (!srcBin) throw new Error("could not locate the built host binary");

  const out = stageDir(key);
  // Stage the binary + runtime libs; exclude webview (header-only) + import libs/symbols.
  const skip = new Set(["webview.dll"]);
  const skipExt = new Set([".lib", ".exp", ".pdb", ".ilk"]);
  for (const f of fs.readdirSync(srcBin)) {
    if (skip.has(f) || skipExt.has(path.extname(f))) continue;
    if (f.startsWith("hull-host") || f.endsWith(".dll") || f.endsWith(".dylib") || f.endsWith(".so")) {
      fs.copyFileSync(path.join(srcBin, f), path.join(out, f));
    }
  }
  console.log(`  staged ${key} -> packages/hull-${key}/bin`);
}

// ---- Linux build via Docker (any host OS) ----
function buildLinuxDocker(key, secure) {
  if (!dockerAvailable()) {
    throw new Error("Docker is not available — install Docker Desktop, or build on Linux / CI.");
  }
  const image = "hull-linux-builder";
  const dockerfile = path.join(hostDir, "linux.Dockerfile");
  console.log("  docker: building image (cached after first run)…");
  execFileSync("docker", ["build", "-t", image, "-f", dockerfile, hostDir], { stdio: "inherit" });

  const out = stageDir(key);
  const vol = (p) => `${p.replace(/\\/g, "/")}`; // Docker Desktop accepts C:/...
  const binOut = secure ? "hull-host-secure" : "hull-host";
  const flags = secure ? "-DHULL_CRYPTO=ON" : "";
  const cmd =
    `cmake -S /work/host -B /tmp/build -DCMAKE_BUILD_TYPE=Release ${flags} && ` +
    `cmake --build /tmp/build -j"$(nproc)" && cp /tmp/build/bin/${binOut} /out/${binOut}`;
  console.log(`  docker: compiling ${binOut}…`);
  execFileSync("docker", [
    "run", "--rm",
    "-v", `${vol(hostDir)}:/work/host:ro`,
    "-v", `${vol(out)}:/out`,
    image, "sh", "-c", cmd,
  ], { stdio: "inherit" });
  console.log(`  staged ${key} (${binOut}) -> packages/hull-${key}/bin`);
}

// ---- decide what to build ----
function canBuildHere(key) {
  const tos = key.split("-")[0];
  if (tos === process.platform) return "native";
  if (tos === "linux" && dockerAvailable()) return "docker";
  return null;
}

function buildOne(key, { strict, secure }) {
  const how = canBuildHere(key);
  if (how === "native") return buildNative(key, secure);
  if (how === "docker") return buildLinuxDocker(key, secure);
  const msg =
    `cannot build ${key} on ${process.platform}.` +
    (key.startsWith("linux") ? " Install Docker, or build on Linux/CI." :
     key.startsWith("darwin") ? " macOS hosts require a Mac (SDK/frameworks) or a CI macOS runner." :
     " Windows hosts require Windows (MSVC/WebView2) or a CI windows runner.");
  if (strict) throw new Error(msg);
  console.warn(`  skip ${key}: ${msg}`);
}

const targets = all ? KNOWN : [normalize(target)];
console.log(`build-host: ${all ? "all reachable targets" : targets.join(", ")}${secure ? " [secure]" : ""} (on ${currentKey})`);
let built = 0;
for (const key of targets) {
  try { buildOne(key, { strict: !all, secure }); built++; }
  catch (e) { if (!all) { console.error(`build-host: ${e.message}`); process.exit(1); } else console.warn(`  skip ${key}: ${e.message}`); }
}
console.log(`build-host: done (${all ? "see notes above for skipped targets" : built + " target(s)"}).`);
