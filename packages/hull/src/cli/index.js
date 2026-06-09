import { dev } from "./dev.js";
import { build } from "./build.js";
import { start } from "./start.js";
import { eject } from "./eject.js";
import { installer } from "./installer.js";

const HELP = `
hull — tiny native desktop apps from your web UI

Usage: hull <command> [options]

  dev        Start the Vite dev server and open it in a native window (HMR)
  build      Build the single-file UI and package it with the native host
  start      Run the packaged app from ./release
  installer  Wrap the build into a native installer (.dmg / .deb / .exe)
  eject      Copy the C++ host project into ./desktop for custom native code
  help       Show this help

Options:
  -v, --verbose   Print per-step timings (every command also prints its total time)

Config is optional. Defaults come from package.json; override in .hullrc:
  { "appId": "com.you.app", "secure": false, "window": { "title": "App" } }
`;

export async function run(argv) {
  const verbose = argv.includes("-v") || argv.includes("--verbose");
  const rest = argv.filter((a) => a !== "-v" && a !== "--verbose");
  const [cmd, ...args] = rest;
  const cwd = process.cwd();
  const opts = { verbose };
  try {
    switch (cmd) {
      case "dev":   await dev(cwd, args, opts);   break;
      case "build": await build(cwd, args, opts); break;
      case "start": await start(cwd, args, opts); break;
      case "installer": await installer(cwd, args, opts); break;
      case "eject": await eject(cwd, args, opts); break;
      case undefined:
      case "help":
      case "-h":
      case "--help":
        console.log(HELP);
        break;
      default:
        console.error(`hull: unknown command "${cmd}"`);
        console.log(HELP);
        process.exit(1);
    }
  } catch (e) {
    console.error(`hull ${cmd ?? ""}: ${e.message}`);
    process.exit(1);
  }
}
