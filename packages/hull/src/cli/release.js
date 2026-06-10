import fs from "node:fs";
import path from "node:path";
import { execFileSync } from "node:child_process";
import archiver from "archiver";

const xmlEscape = (s) =>
  String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

const VERSION_RE = /^v\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/;

// Files we never ship: webview is header-only (the host doesn't import webview.dll),
// plus import libs / debug symbols.
const DENY_NAMES = new Set(["webview.dll"]);
const DENY_EXT = new Set([".lib", ".exp", ".pdb", ".ilk"]);

export function sanitize(name) {
  return name.replace(/[^\w.-]+/g, "-").replace(/^-+|-+$/g, "") || "app";
}

// Validate and normalize the version arg. Empty -> { label: "development" }.
export function parseVersion(version) {
  if (!version) return { version: null, label: "development" };
  if (!VERSION_RE.test(version)) {
    throw new Error(`invalid version "${version}" — expected vX.Y.Z (e.g. v1.2.3)`);
  }
  return { version, label: version };
}

const isWin = (key) => key.startsWith("win32-");

// Copy the minimal runtime files into the bundle: the chosen host binary (`binName`)
// plus its runtime libs (DLLs). The other flavor's binary, webview.dll, import libs
// and debug symbols are skipped.
export function copyHostFiles(hostDir, destDir, binName) {
  fs.mkdirSync(destDir, { recursive: true });
  for (const entry of fs.readdirSync(hostDir)) {
    if (DENY_NAMES.has(entry) || DENY_EXT.has(path.extname(entry))) continue;
    if (entry.startsWith("hull-host") && entry !== binName) continue; // skip other flavor
    const dest = path.join(destDir, entry);
    fs.copyFileSync(path.join(hostDir, entry), dest);
    // The source may have lost its exec bit (npm tarballs packed in CI); the
    // archives force 0o755 already — keep the loose bundle dir runnable too.
    if (entry === binName && !binName.endsWith(".exe")) {
      try { fs.chmodSync(dest, 0o755); } catch { /* best effort */ }
    }
  }
}

// Write a double-clickable launcher appropriate to the TARGET os, invoking `binName`.
export function writeLauncher(destDir, key, cfg, binName, iconName) {
  const os = key.split("-")[0];
  if (os === "win32") {
    const name = `${sanitize(cfg.title)}.cmd`;
    const icon = iconName ? ` --icon "%~dp0${iconName}"` : "";
    const body =
      `@echo off\r\n` +
      `"%~dp0${binName}" --app "%~dp0app.html" --title "${cfg.title}" ` +
      `--app-id "${cfg.appId}" --width ${cfg.width} --height ${cfg.height}${icon}\r\n`;
    fs.writeFileSync(path.join(destDir, name), body);
    return { name, exec: false };
  }
  // macOS uses .command (double-clickable in Finder); Linux uses .sh.
  const ext = os === "darwin" ? "command" : "sh";
  const name = `${sanitize(cfg.title)}.${ext}`;
  const icon = iconName ? ` --icon "$DIR/${iconName}"` : "";
  // Linux WebKitGTK sandbox control (no-op on macOS). When unset, the host auto-detects.
  let sandbox = "";
  if (os === "linux") {
    if (cfg.linuxSandbox === false) sandbox = `export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1\n`;
    else if (cfg.linuxSandbox === true) sandbox = `export HULL_FORCE_SANDBOX=1\n`;
  }
  const body =
    `#!/bin/sh\n` +
    `DIR="$(cd "$(dirname "$0")" && pwd)"\n` +
    sandbox +
    `"$DIR/${binName}" --app "$DIR/app.html" --title "${cfg.title}" ` +
    `--app-id "${cfg.appId}" --width ${cfg.width} --height ${cfg.height}${icon}\n`;
  fs.writeFileSync(path.join(destDir, name), body, { mode: 0o755 });
  return { name, exec: true };
}

// Build a macOS .app bundle inside `bundleDir`. The generic prebuilt host stays the
// executable; a tiny CFBundleExecutable launcher script execs it with the app's args,
// and the icon comes from the bundle (CFBundleIconFile) — macOS draws the Dock/Finder
// icon from the bundle, not at runtime. Returns the .app folder name.
//
// Note: the host links Homebrew OpenSSL dylibs by absolute path, so the .app runs on
// machines that have those (the build machine). Bundling+relinking the dylibs and code
// signing/notarization for distribution to other Macs is a separate, later step.
export function writeMacApp(bundleDir, cfg, hostDir, binName, builtHtml, iconPath) {
  const appName = `${sanitize(cfg.title)}.app`;
  const contents = path.join(bundleDir, appName, "Contents");
  const macosDir = path.join(contents, "MacOS");
  const resDir = path.join(contents, "Resources");
  fs.mkdirSync(macosDir, { recursive: true });
  fs.mkdirSync(resDir, { recursive: true });

  // host binary + runtime libs -> Contents/MacOS ; UI -> Contents/Resources
  copyHostFiles(hostDir, macosDir, binName);
  try { fs.chmodSync(path.join(macosDir, binName), 0o755); } catch { /* best effort */ }
  fs.copyFileSync(builtHtml, path.join(resDir, "app.html"));

  // Icon: copy the PNG and try to make an .icns (sips, macOS only). CFBundleIconFile is
  // set only if the .icns was produced (a PNG alone won't render as the app icon).
  let iconKey = "";
  if (iconPath && fs.existsSync(iconPath)) {
    fs.copyFileSync(iconPath, path.join(resDir, "icon.png"));
    try {
      execFileSync("sips", ["-s", "format", "icns", iconPath, "--out",
                            path.join(resDir, "icon.icns")], { stdio: "ignore" });
      iconKey = "  <key>CFBundleIconFile</key><string>icon</string>\n";
    } catch { /* sips unavailable (non-mac build host) -> ship without a bundle icon */ }
  }

  // CFBundleExecutable: a launcher that execs the host with this app's args.
  const execName = sanitize(cfg.title);
  const script =
    `#!/bin/sh\n` +
    `DIR="$(cd "$(dirname "$0")" && pwd)"\n` +
    `RES="$DIR/../Resources"\n` +
    `exec "$DIR/${binName}" --app "$RES/app.html" --title "${cfg.title}" ` +
    `--app-id "${cfg.appId}" --width ${cfg.width} --height ${cfg.height}\n`;
  fs.writeFileSync(path.join(macosDir, execName), script, { mode: 0o755 });
  try { fs.chmodSync(path.join(macosDir, execName), 0o755); } catch { /* best effort */ }

  const plist =
    `<?xml version="1.0" encoding="UTF-8"?>\n` +
    `<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n` +
    `<plist version="1.0">\n<dict>\n` +
    `  <key>CFBundleName</key><string>${xmlEscape(cfg.title)}</string>\n` +
    `  <key>CFBundleDisplayName</key><string>${xmlEscape(cfg.title)}</string>\n` +
    `  <key>CFBundleIdentifier</key><string>${xmlEscape(cfg.appId)}</string>\n` +
    `  <key>CFBundleExecutable</key><string>${xmlEscape(execName)}</string>\n` +
    `  <key>CFBundlePackageType</key><string>APPL</string>\n` +
    `  <key>CFBundleVersion</key><string>1.0</string>\n` +
    `  <key>CFBundleShortVersionString</key><string>1.0</string>\n` +
    `  <key>NSHighResolutionCapable</key><true/>\n` +
    iconKey +
    `</dict>\n</plist>\n`;
  fs.writeFileSync(path.join(contents, "Info.plist"), plist);
  return { appName };
}

// tar.gz a directory tree (the .app), preserving on-disk file modes (exec bits).
export function makeAppArchive(srcDir, outFile, rootName) {
  return new Promise((resolve, reject) => {
    const output = fs.createWriteStream(outFile);
    const archive = archiver("tar", { gzip: true });
    output.on("close", () => resolve(archive.pointer()));
    archive.on("error", reject);
    archive.pipe(output);
    archive.directory(srcDir, rootName);
    archive.finalize();
  });
}

// Default archive format per target: zip for Windows, tar.gz for unix.
export function defaultFormat(key) {
  return isWin(key) ? "zip" : "tar.gz";
}

// Create an archive whose single top-level folder is `rootName`. On unix targets,
// the binary + shell launcher are marked executable (0755) regardless of build host.
export function makeArchive(srcDir, outFile, format, rootName, key, launcherName, binName) {
  return new Promise((resolve, reject) => {
    const output = fs.createWriteStream(outFile);
    const archive =
      format === "zip"
        ? archiver("zip", { zlib: { level: 9 } })
        : archiver("tar", { gzip: true });
    const execNames = isWin(key) ? [] : [binName, launcherName];

    output.on("close", () => resolve(archive.pointer()));
    archive.on("error", reject);
    archive.pipe(output);

    for (const entry of fs.readdirSync(srcDir)) {
      const opts = { name: `${rootName}/${entry}` };
      if (execNames.includes(entry)) opts.mode = 0o755;
      archive.file(path.join(srcDir, entry), opts);
    }
    archive.finalize();
  });
}
