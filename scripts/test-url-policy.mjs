// Compile + run the standalone C++ url_policy test (no CMake needed — the header
// is dependency-free). Mirrors the manual command documented in the test file.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const hostDir = path.join(root, "packages", "hull", "host");
const out = path.join(os.tmpdir(), `hull-url-policy-test${process.platform === "win32" ? ".exe" : ""}`);

function compiler() {
  for (const c of ["c++", "clang++", "g++"]) {
    try { execFileSync(c, ["--version"], { stdio: "ignore" }); return c; }
    catch { /* next */ }
  }
  return null;
}

const cc = compiler();
if (!cc) {
  console.error("test-url-policy: no C++ compiler found (c++/clang++/g++) — skipping is not allowed; install a toolchain.");
  process.exit(1);
}

execFileSync(cc, [
  "-std=c++17", "-I", path.join(hostDir, "src"),
  path.join(hostDir, "test", "url_policy_test.cpp"), "-o", out,
], { stdio: "inherit" });
execFileSync(out, [], { stdio: "inherit" });
fs.rmSync(out, { force: true });
