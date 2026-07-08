// Optional code-signing hooks — every function is a NO-OP unless the matching
// `.hullrc` `sign` config is present, so unsigned builds keep working untouched.
//
//   "sign": {
//     "mac": {
//       "identity": "Developer ID Application: You (TEAMID)",
//       // PREFERRED — a keychain profile stored once with
//       //   xcrun notarytool store-credentials <name> --apple-id … --team-id … --password …
//       // keeps the secret out of argv entirely:
//       "notarize": { "keychainProfile": "hull-notary" }
//       // Fallback (secret on the command line — see the note below):
//       // "notarize": { "appleId": "you@example.com", "teamId": "TEAMID",
//       //               "passwordEnv": "NOTARY_PASSWORD" }
//     },
//     "windows": { "certFile": "certs/app.pfx", "passwordEnv": "WIN_CERT_PASSWORD",
//                  "timestampUrl": "http://timestamp.digicert.com" }
//   }
//
// Secrets come from ENV VARS named in the config (passwordEnv) — never from the
// config file itself. NOTE: when a *password* is used (notarize.passwordEnv,
// windows.passwordEnv) the signing tool receives it as a command-line argument,
// which is briefly visible to `ps` / Task Manager by other local users. Prefer
// notarize.keychainProfile on macOS (no secret on argv); on Windows, a cert in
// the machine store avoids /p entirely. Signing tools must run on their own OS
// (codesign/xcrun on macOS, signtool on Windows); when the tool is missing the
// build fails loudly rather than shipping an artifact the developer believes is
// signed.
import { execFileSync } from "node:child_process";

const run = (cmd, args, label) => {
  try {
    execFileSync(cmd, args, { stdio: "inherit" });
  } catch (e) {
    throw new Error(`${label} failed (${cmd}): ${e.message}`);
  }
};

const envSecret = (name, label) => {
  if (!name) return null;
  const v = process.env[name];
  if (!v) throw new Error(`${label}: env var ${name} is not set`);
  return v;
};

// Sign a macOS .app bundle (Developer ID + hardened runtime).
export function signMacApp(cfg, appPath) {
  const identity = cfg.sign?.mac?.identity;
  if (!identity) return false;
  if (process.platform !== "darwin") {
    throw new Error("sign.mac.identity is set but this build is not running on macOS");
  }
  console.log(`hull sign: codesign ${appPath}`);
  run("codesign", ["--deep", "--force", "--options", "runtime",
                   "--sign", identity, appPath], "codesign");
  return true;
}

// Sign a .dmg (same identity as the app).
export function signMacFile(cfg, filePath) {
  const identity = cfg.sign?.mac?.identity;
  if (!identity) return false;
  console.log(`hull sign: codesign ${filePath}`);
  run("codesign", ["--force", "--sign", identity, filePath], "codesign");
  return true;
}

// Submit to Apple's notary service and staple the ticket (blocks until done).
export function notarizeMac(cfg, filePath) {
  const n = cfg.sign?.mac?.notarize;
  if (!n) return false;
  console.log(`hull sign: notarizing ${filePath} (this can take a few minutes)…`);
  // Prefer a stored keychain profile — no secret on the command line.
  const args = ["notarytool", "submit", filePath, "--wait"];
  if (n.keychainProfile) {
    args.push("--keychain-profile", n.keychainProfile);
  } else {
    const password = envSecret(n.passwordEnv, "notarize");
    args.push("--apple-id", n.appleId, "--team-id", n.teamId, "--password", password);
  }
  run("xcrun", args, "notarytool");
  run("xcrun", ["stapler", "staple", filePath], "stapler");
  return true;
}

// Authenticode-sign a Windows artifact (installer .exe).
export function signWindows(cfg, filePath) {
  const w = cfg.sign?.windows;
  if (!w?.certFile) return false;
  if (process.platform !== "win32") {
    throw new Error("sign.windows.certFile is set but this build is not running on Windows");
  }
  const args = ["sign", "/f", w.certFile, "/fd", "sha256",
                "/tr", w.timestampUrl ?? "http://timestamp.digicert.com", "/td", "sha256"];
  const password = w.passwordEnv ? envSecret(w.passwordEnv, "signtool") : null;
  if (password) args.push("/p", password);
  args.push(filePath);
  console.log(`hull sign: signtool ${filePath}`);
  run("signtool", args, "signtool");
  return true;
}
