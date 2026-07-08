// Unit tests for the CLI config + launcher plumbing (fullscreen flag path).
// Run: npm -w @mwguerra/hull run test   (node --test, no extra deps)
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { loadConfig, hostArgs, windowFlags } from "../src/cli/config.js";
import { writeLauncher, writeMacApp } from "../src/cli/release.js";
import { debLauncherBody } from "../src/cli/installer.js";
import { compareVersions } from "../src/bridge/version.js";

function tmpProject(files = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "hull-test-"));
  fs.writeFileSync(path.join(dir, "package.json"), JSON.stringify({ name: "demo-app" }));
  for (const [name, content] of Object.entries(files)) {
    fs.writeFileSync(path.join(dir, name), typeof content === "string" ? content : JSON.stringify(content));
  }
  return dir;
}

test("fullscreen defaults to false and emits no --fullscreen flag", async () => {
  const cwd = tmpProject();
  const cfg = await loadConfig(cwd);
  assert.equal(cfg.fullscreen, false);
  assert.ok(!hostArgs(cfg).includes("--fullscreen"));
});

test("window.fullscreen=true flows into cfg and hostArgs", async () => {
  const cwd = tmpProject({ ".hullrc": { window: { fullscreen: true } } });
  const cfg = await loadConfig(cwd);
  assert.equal(cfg.fullscreen, true);
  const args = hostArgs(cfg);
  assert.ok(args.includes("--fullscreen"));
  // deep-merge must keep the other window defaults
  assert.equal(cfg.width, 1100);
  assert.equal(cfg.height, 760);
});

test("window.fullscreen coerces truthy/falsy values", async () => {
  for (const [value, expected] of [[false, false], [0, false], [1, true], ["yes", true]]) {
    const cwd = tmpProject({ ".hullrc": { window: { fullscreen: value } } });
    const cfg = await loadConfig(cwd);
    assert.equal(cfg.fullscreen, expected, `fullscreen: ${JSON.stringify(value)}`);
  }
});

test("window options flow through windowFlags into hostArgs and launchers", async () => {
  const cwd = tmpProject({
    ".hullrc": {
      singleInstance: true,
      window: {
        fullscreen: true, minWidth: 400, minHeight: 300, maxWidth: 1600,
        alwaysOnTop: true, center: true, rememberState: true,
      },
    },
  });
  const cfg = await loadConfig(cwd);
  const flags = windowFlags(cfg);
  for (const expected of ["--fullscreen", "--min-width", "--min-height", "--max-width",
                          "--always-on-top", "--center", "--remember-state",
                          "--single-instance"]) {
    assert.ok(flags.includes(expected), `windowFlags carries ${expected}`);
  }
  assert.ok(!flags.includes("--max-height"), "unset maxHeight emits no flag");
  assert.equal(flags[flags.indexOf("--min-width") + 1], "400");
  // hostArgs (dev/start) and the launcher writers must carry the same set
  const args = hostArgs(cfg);
  for (const f of flags) assert.ok(args.includes(f), `hostArgs carries ${f}`);
  const dir = tmpProject();
  const { name } = writeLauncher(dir, "linux-x64", cfg, "hull-host", null);
  const body = fs.readFileSync(path.join(dir, name), "utf8");
  for (const f of flags) assert.ok(body.includes(f), `launcher carries ${f}`);
  // The .deb /usr/bin launcher is a distinct 5th path — it once dropped these.
  const debBody = debLauncherBody("demo-app", "hull-host", cfg);
  for (const f of flags) assert.ok(debBody.includes(f), `.deb launcher carries ${f}`);
});

test("compareVersions orders releases and prereleases (semver §11)", () => {
  const gt = (a, b) => assert.equal(compareVersions(a, b), 1, `${a} > ${b}`);
  const eq = (a, b) => assert.equal(compareVersions(a, b), 0, `${a} == ${b}`);
  gt("1.2.0", "1.1.9");
  gt("1.10.0", "1.9.0");        // numeric, not lexical
  gt("v2.0.0", "v1.9.9");       // leading v tolerated
  eq("1.0.0", "1.0.0");
  eq("1.0", "1.0.0");           // missing patch == 0
  // a prerelease is LOWER than its release (the bug the adversarial pass caught)
  gt("1.0.0", "1.0.0-beta");
  assert.equal(compareVersions("1.0.0-beta", "1.0.0"), -1);
  gt("1.0.0-beta", "1.0.0-alpha");
  gt("1.0.0-beta.2", "1.0.0-beta.1");
  gt("1.0.0-rc.1", "1.0.0-beta"); // alphanumeric ordering, longer wins on shared prefix
  assert.equal(compareVersions("1.0.0-alpha", "1.0.0-alpha.1"), -1); // shorter is lower
});

const baseCfg = {
  title: "Demo", appId: "com.hull.demo", width: 800, height: 600,
  fullscreen: true, linuxSandbox: undefined,
};

test("win32 launcher bakes --fullscreen when configured", () => {
  const dir = tmpProject();
  const { name } = writeLauncher(dir, "win32-x64", baseCfg, "hull-host.exe", null);
  const body = fs.readFileSync(path.join(dir, name), "utf8");
  assert.match(body, / --fullscreen/);
});

test("linux launcher bakes --fullscreen when configured", () => {
  const dir = tmpProject();
  const { name } = writeLauncher(dir, "linux-x64", baseCfg, "hull-host", null);
  const body = fs.readFileSync(path.join(dir, name), "utf8");
  assert.match(body, / --fullscreen/);
});

test("launchers omit --fullscreen when not configured", () => {
  const dir = tmpProject();
  const cfg = { ...baseCfg, fullscreen: false };
  const win = writeLauncher(dir, "win32-x64", cfg, "hull-host.exe", null);
  const lin = writeLauncher(dir, "linux-x64", cfg, "hull-host", null);
  assert.doesNotMatch(fs.readFileSync(path.join(dir, win.name), "utf8"), /--fullscreen/);
  assert.doesNotMatch(fs.readFileSync(path.join(dir, lin.name), "utf8"), /--fullscreen/);
});

test("macOS .app launcher bakes --fullscreen and forwards extra args", () => {
  const dir = tmpProject();
  const hostDir = tmpProject();
  fs.writeFileSync(path.join(hostDir, "hull-host"), "#!/bin/sh\n");
  const html = path.join(dir, "index.html");
  fs.writeFileSync(html, "<!doctype html>");
  writeMacApp(dir, baseCfg, hostDir, "hull-host", html, null);
  const launcher = path.join(dir, "Demo.app", "Contents", "MacOS", "Demo");
  const body = fs.readFileSync(launcher, "utf8");
  assert.match(body, / --fullscreen/);
  assert.match(body, /"\$@"/); // `open <app> --args --fullscreen` must reach the host
});
