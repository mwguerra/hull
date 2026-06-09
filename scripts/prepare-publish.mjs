// Inject the per-platform host packages as os/cpu-gated optionalDependencies into
// @mwguerra/hull's manifest, right before `npm publish`. Kept out of the repo
// manifest because npm validates workspace os/cpu and would reject non-matching
// platforms during local install. Run in CI on an ephemeral checkout (it rewrites
// package.json in place); pass the version to pin to.
//
//   node scripts/prepare-publish.mjs [version]   (default: the package's own version)

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const pkgPath = path.join(root, "packages", "hull", "package.json");
const TARGETS = ["win32-x64", "darwin-arm64", "linux-x64"];

const pkg = JSON.parse(fs.readFileSync(pkgPath, "utf8"));
const version = process.argv[2] ?? pkg.version;

delete pkg["comment:optionalDependencies"];
pkg.optionalDependencies = Object.fromEntries(
  TARGETS.map((k) => [`@mwguerra/hull-${k}`, version])
);

fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + "\n");
console.log(`prepare-publish: pinned optionalDependencies to ${version}`);
for (const k of TARGETS) console.log(`  @mwguerra/hull-${k}@${version}`);
