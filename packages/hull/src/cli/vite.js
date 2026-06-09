import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import path from "node:path";

// Load the *project's own* Vite (resolved from the app's node_modules) so the
// version always matches the user's framework plugins. Vite is a peer dependency.
export async function loadVite(cwd) {
  const require = createRequire(pathToFileURL(path.join(cwd, "package.json")).href);
  let vitePath;
  try {
    vitePath = require.resolve("vite");
  } catch {
    throw new Error('Vite was not found in this project. Install it with "npm i -D vite".');
  }
  return import(pathToFileURL(vitePath).href);
}
