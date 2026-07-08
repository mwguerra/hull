// Unit tests for the CLI config + launcher plumbing (fullscreen flag path).
// Run: npm -w @mwguerra/hull run test   (node --test, no extra deps)
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { loadConfig, hostArgs } from "../src/cli/config.js";
import { writeLauncher, writeMacApp } from "../src/cli/release.js";

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
