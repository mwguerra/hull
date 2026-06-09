import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { execFileSync } from "node:child_process";
import { loadConfig } from "./config.js";
import { currentTarget, binaryName } from "./host.js";
import { parseVersion, sanitize } from "./release.js";
import { createTimer } from "./timing.js";

// hull installer [vX.Y.Z]
// Wrap the already-built bundle for the CURRENT platform into a native installer:
//   macOS   -> .dmg   (hdiutil; the .app + an Applications drop-link)
//   Linux   -> .deb   (dpkg-deb; installs to /opt + a .desktop + icon, declares deps)
//   Windows -> .exe   (Inno Setup; per-user install, Start Menu/Desktop shortcuts)
// Each runs on its own OS (the packaging tools are OS-native). Run `hull build` first.
export async function installer(cwd, args, { verbose } = {}) {
  const timer = createTimer(verbose);
  const version = args.find((a) => !a.startsWith("-")) ?? null;
  const { label } = parseVersion(version);
  const cfg = await loadConfig(cwd);
  const key = currentTarget();
  const outDir = path.join(cwd, cfg.releaseDir, label);
  const bundleDir = path.join(outDir, key + (cfg.secure ? "-secure" : ""));
  if (!fs.existsSync(bundleDir)) {
    const v = label === "development" ? "" : " " + label;
    throw new Error(`no build at ${path.relative(cwd, bundleDir)}. Run "hull build${v}" first.`);
  }
  const base = `${sanitize(cfg.title)}-${label}-${key}${cfg.secure ? "-secure" : ""}`;
  const ver = label === "development" ? "0.0.0" : label.replace(/^v/, ""); // installer version
  const binName = binaryName(key, cfg.secure); // hull-host[-secure][.exe]
  timer.step("located build");

  let out;
  if (process.platform === "darwin") out = macDmg(bundleDir, outDir, base, cfg);
  else if (process.platform === "win32") out = winInno(bundleDir, outDir, base, cfg, ver, binName);
  else out = linuxDeb(bundleDir, outDir, base, cfg, key, ver, binName);

  const rel = path.relative(cwd, out).replace(/\\/g, "/");
  console.log(`\nhull installer: ${rel}  (${(fs.statSync(out).size / 1048576).toFixed(1)} MB)`);
  timer.total("hull installer");
}

// ---------------------------------- macOS (.dmg) ----------------------------------
function macDmg(bundleDir, outDir, base, cfg) {
  const appName = `${sanitize(cfg.title)}.app`;
  const appPath = path.join(bundleDir, appName);
  if (!fs.existsSync(appPath)) {
    throw new Error(`no ${appName} in the build — run "hull build" on macOS first.`);
  }
  const stage = path.join(outDir, ".dmg-stage");
  fs.rmSync(stage, { recursive: true, force: true });
  fs.mkdirSync(stage, { recursive: true });
  execFileSync("cp", ["-R", appPath, path.join(stage, appName)]);
  try { fs.symlinkSync("/Applications", path.join(stage, "Applications")); } catch { /* exists */ }

  const dmg = path.join(outDir, `${base}.dmg`);
  fs.rmSync(dmg, { force: true });
  execFileSync("hdiutil", ["create", "-volname", cfg.title, "-srcfolder", stage,
                           "-ov", "-format", "UDZO", dmg], { stdio: "inherit" });
  fs.rmSync(stage, { recursive: true, force: true });
  return dmg;
}

// ---------------------------------- Linux (.deb) ----------------------------------
function debArch(key) {
  return key.endsWith("-arm64") ? "arm64" : "amd64";
}
function debName(cfg) {
  // Debian package names: lowercase letters, digits, '+', '-', '.'; start alphanumeric.
  const base = (cfg.appId?.split(".").pop() || sanitize(cfg.title)).toLowerCase();
  return base.replace(/[^a-z0-9+.-]+/g, "-").replace(/^[^a-z0-9]+/, "") || "hull-app";
}
// Resolve shared-library Depends from the binary using dpkg-shlibdeps (part of
// dpkg-dev, present on any host that built the C++ host). Version-proof: it emits the
// exact runtime package names for the running distro. Falls back if unavailable.
function computeDebDeps(stage, pkg, binName, cfg) {
  try {
    const debianDir = path.join(stage, "debian");
    fs.mkdirSync(debianDir, { recursive: true });
    fs.writeFileSync(path.join(debianDir, "control"),
      `Source: ${pkg}\nMaintainer: ${cfg.appId}\n\nPackage: ${pkg}\nArchitecture: any\n`);
    const out = execFileSync("dpkg-shlibdeps", ["-O", path.join("opt", pkg, binName)],
      { cwd: stage, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
    fs.rmSync(debianDir, { recursive: true, force: true });
    const m = /shlibs:Depends=(.+)/.exec(out);
    if (m && m[1].trim()) return m[1].trim();
  } catch { /* dpkg-shlibdeps not available — use the fallback below */ }
  const base = "libwebkitgtk-6.0-4, libgtk-4-1, libsecret-1-0, libcups2";
  return cfg.secure ? `${base}, libsqlcipher0` : base;
}

const xmlEsc = (s) =>
  String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

// Write /usr/share/metainfo/<appId>.metainfo.xml (AppStream) so App Center shows the
// name, summary, license, developer, icon, and version/date instead of "Unknown".
function writeMetainfo(stage, cfg, ver) {
  const dir = path.join(stage, "usr", "share", "metainfo");
  fs.mkdirSync(dir, { recursive: true });
  const summary = (cfg.description || cfg.title).split("\n")[0].slice(0, 80);
  const devId = cfg.appId.split(".").slice(0, 2).join(".") || cfg.appId; // e.g. com.you
  const devName = cfg.publisher || devId;
  const date = new Date().toISOString().slice(0, 10);
  const xml =
`<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>${cfg.appId}</id>
  <name>${xmlEsc(cfg.title)}</name>
  <summary>${xmlEsc(summary)}</summary>
  <metadata_license>CC0-1.0</metadata_license>
${cfg.license ? `  <project_license>${xmlEsc(cfg.license)}</project_license>\n` : ""}  <description><p>${xmlEsc(cfg.description || cfg.title)}</p></description>
  <launchable type="desktop-id">${cfg.appId}.desktop</launchable>
  <icon type="stock">${cfg.appId}</icon>
  <developer id="${xmlEsc(devId)}"><name>${xmlEsc(devName)}</name></developer>
  <developer_name>${xmlEsc(devName)}</developer_name>
  <content_rating type="oars-1.1"/>
  <releases>
    <release version="${ver}" date="${date}"/>
  </releases>
</component>
`;
  fs.writeFileSync(path.join(dir, `${cfg.appId}.metainfo.xml`), xml);
}

function linuxDeb(bundleDir, outDir, base, cfg, key, ver, binName) {
  const pkg = debName(cfg);
  const stage = path.join(outDir, ".deb-stage");
  fs.rmSync(stage, { recursive: true, force: true });

  // Payload -> /opt/<pkg>
  const opt = path.join(stage, "opt", pkg);
  fs.mkdirSync(opt, { recursive: true });
  for (const e of fs.readdirSync(bundleDir)) {
    fs.cpSync(path.join(bundleDir, e), path.join(opt, e), { recursive: true });
  }
  fs.chmodSync(path.join(opt, binName), 0o755);

  // Launcher -> /usr/bin/<pkg>
  const usrbin = path.join(stage, "usr", "bin");
  fs.mkdirSync(usrbin, { recursive: true });
  fs.writeFileSync(path.join(usrbin, pkg),
    `#!/bin/sh\n` +
    `exec /opt/${pkg}/${binName} --app /opt/${pkg}/app.html --title "${cfg.title}" ` +
    `--app-id "${cfg.appId}" --width ${cfg.width} --height ${cfg.height} ` +
    `--icon /opt/${pkg}/icon.png "$@"\n`, { mode: 0o755 });

  // Desktop entry + icon -> the compositor matches the window (app-id) to this .desktop
  const apps = path.join(stage, "usr", "share", "applications");
  fs.mkdirSync(apps, { recursive: true });
  const hasIcon = fs.existsSync(path.join(bundleDir, "icon.png"));
  fs.writeFileSync(path.join(apps, `${cfg.appId}.desktop`),
    `[Desktop Entry]\nType=Application\nName=${cfg.title}\nExec=/usr/bin/${pkg}\n` +
    (hasIcon ? `Icon=${cfg.appId}\n` : "") +
    `StartupWMClass=${cfg.appId}\nTerminal=false\nCategories=Utility;\n`);
  if (hasIcon) {
    const icons = path.join(stage, "usr", "share", "icons", "hicolor", "256x256", "apps");
    fs.mkdirSync(icons, { recursive: true });
    fs.copyFileSync(path.join(bundleDir, "icon.png"), path.join(icons, `${cfg.appId}.png`));
  }

  // AppStream MetaInfo -> rich metadata in GNOME Software / App Center (name, summary,
  // license, developer, version/date, icon). Without it those fields show "Unknown".
  writeMetainfo(stage, cfg, ver);

  // Control metadata. Compute Depends from the actual binary via dpkg-shlibdeps so the
  // package names are correct for THIS distro (Ubuntu 24.04 renamed many libs in the
  // t64 transition); fall back to a best-effort list if dpkg-dev isn't installed.
  const depends = computeDebDeps(stage, pkg, binName, cfg);
  const desc = cfg.description || "A desktop app packaged with Hull.";
  const deb = path.join(stage, "DEBIAN");
  fs.mkdirSync(deb, { recursive: true });
  fs.writeFileSync(path.join(deb, "control"),
    `Package: ${pkg}\nVersion: ${ver}\nArchitecture: ${debArch(key)}\n` +
    `Maintainer: ${cfg.publisher || cfg.appId}\nDepends: ${depends}\nSection: utils\n` +
    `Priority: optional\nDescription: ${cfg.title}\n ${desc}\n`);

  const outFile = path.join(outDir, `${base}.deb`);
  fs.rmSync(outFile, { force: true });
  execFileSync("dpkg-deb", ["--build", "--root-owner-group", stage, outFile], { stdio: "inherit" });
  fs.rmSync(stage, { recursive: true, force: true });
  return outFile;
}

// ---------------------------------- Windows (.exe) ----------------------------------
function findISCC() {
  const local = process.env.LOCALAPPDATA || path.join(process.env.USERPROFILE || "", "AppData", "Local");
  for (const p of [
    "C:/Program Files (x86)/Inno Setup 6/ISCC.exe",
    "C:/Program Files/Inno Setup 6/ISCC.exe",
    path.join(local, "Programs", "Inno Setup 6", "ISCC.exe"), // winget per-user install
  ]) if (fs.existsSync(p)) return p;
  return null;
}

// Wrap a PNG into a (Vista+) PNG-compressed .ico so Explorer/shortcuts show the logo.
function pngToIco(png) {
  const w = png.readUInt32BE(16), h = png.readUInt32BE(20); // PNG IHDR width/height
  const head = Buffer.alloc(22);
  head.writeUInt16LE(0, 0); head.writeUInt16LE(1, 2); head.writeUInt16LE(1, 4); // dir: type=icon, count=1
  head.writeUInt8(w >= 256 ? 0 : w, 6); head.writeUInt8(h >= 256 ? 0 : h, 7);   // 0 means 256
  head.writeUInt8(0, 8); head.writeUInt8(0, 9);
  head.writeUInt16LE(1, 10); head.writeUInt16LE(32, 12);  // planes=1, bpp=32
  head.writeUInt32LE(png.length, 14); head.writeUInt32LE(22, 18); // size, offset
  return Buffer.concat([head, png]);
}

function winInno(bundleDir, outDir, base, cfg, ver, binName) {
  const iscc = findISCC();
  if (!iscc) {
    throw new Error(
      "Inno Setup not found. Install it once, then re-run:\n" +
      "  winget install JRSoftware.InnoSetup");
  }
  // App icon for shortcuts (from the bundled icon.png).
  const png = path.join(bundleDir, "icon.png");
  let icoLine = "";
  if (fs.existsSync(png)) {
    fs.writeFileSync(path.join(bundleDir, "icon.ico"), pngToIco(fs.readFileSync(png)));
    icoLine = `IconFilename: "{app}\\icon.ico"`;
  }
  const guid = crypto.createHash("md5").update(cfg.appId).digest("hex").toUpperCase();
  // GUID without braces; the .iss writes `{{<guid>}` so Inno stores it as `{<guid>}`.
  const g = `${guid.slice(0, 8)}-${guid.slice(8, 12)}-${guid.slice(12, 16)}-${guid.slice(16, 20)}-${guid.slice(20)}`;
  // Inno treats `{` as a constant delimiter (escape as `{{`); inside quoted values `"`
  // is doubled. Keeps a title/appId containing { or " from breaking the generated .iss.
  const issStr = (s) => String(s).replace(/\{/g, "{{");
  const t = issStr(cfg.title);                       // for unquoted directives
  const tq = t.replace(/"/g, '""');                  // for quoted contexts
  const appIdq = issStr(cfg.appId).replace(/"/g, '""');
  const params =
    `--app ""{app}\\app.html"" --title ""${tq}"" --app-id ""${appIdq}"" ` +
    `--width ${cfg.width} --height ${cfg.height} --icon ""{app}\\icon.png""`;

  const iss =
`[Setup]
AppId={{${g}}
AppName=${t}
AppVersion=${ver}
DefaultDirName={autopf}\\${sanitize(cfg.title)}
DefaultGroupName=${t}
UninstallDisplayIcon={app}\\${binName}
OutputDir=${outDir}
OutputBaseFilename=${base}
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible

[Files]
Source: "${bundleDir}\\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\\${tq}"; Filename: "{app}\\${binName}"; Parameters: "${params}"; ${icoLine}
Name: "{autodesktop}\\${tq}"; Filename: "{app}\\${binName}"; Parameters: "${params}"; ${icoLine}
Name: "{group}\\Uninstall ${tq}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\\${binName}"; Parameters: "${params}"; Description: "Launch ${tq}"; Flags: nowait postinstall skipifsilent
`;
  const issPath = path.join(outDir, `${base}.iss`);
  fs.writeFileSync(issPath, iss);
  execFileSync(iscc, [issPath], { stdio: "inherit" });
  fs.rmSync(issPath, { force: true });
  return path.join(outDir, `${base}.exe`);
}
