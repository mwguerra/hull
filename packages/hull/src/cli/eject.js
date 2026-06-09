import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createTimer } from "./timing.js";

// Copy the C++ host project into ./desktop so you can add custom native bindings
// and compile your own host. The standard bindings (HTTP/storage/keychain/print)
// are already there to extend. Requires a C++ toolchain (see desktop/README.md).
export async function eject(cwd, _args, { verbose } = {}) {
  const timer = createTimer(verbose);
  const here = path.dirname(fileURLToPath(import.meta.url));
  const hostSrc = path.resolve(here, "../../host"); // packages/hull/host
  const dest = path.join(cwd, "desktop");

  if (!fs.existsSync(hostSrc)) {
    throw new Error(`bundled host sources not found at ${hostSrc}`);
  }
  if (fs.existsSync(dest)) {
    throw new Error(`${path.relative(cwd, dest)} already exists — remove it first`);
  }
  timer.step("located host sources");

  copyDir(hostSrc, dest);
  timer.step("copied host project");
  console.log(`hull eject: C++ host copied to ./desktop`);
  console.log(`  add bindings under desktop/src/bindings, then build with CMake (see desktop/README.md).`);
  timer.total("hull eject");
}

function copyDir(src, dest) {
  fs.mkdirSync(dest, { recursive: true });
  for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
    if (entry.name === "build" || entry.name === "node_modules") continue; // skip artifacts
    const s = path.join(src, entry.name);
    const d = path.join(dest, entry.name);
    if (entry.isDirectory()) copyDir(s, d);
    else fs.copyFileSync(s, d);
  }
}
