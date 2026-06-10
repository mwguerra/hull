# Distribution

Two audiences: **app developers** shipping a Hull app, and **maintainers**
publishing the prebuilt host packages.

---

## Shipping your app

### Versioned, archived releases

`hull build [version]` produces a versioned, self-contained bundle and an archive:

```bash
hull build            # -> release/development/<platform>/   + archive
hull build v1.2.3     # -> release/v1.2.3/<platform>/        + archive
```

- The version is a positional argument and must match **`vX.Y.Z`** (optionally
  `-suffix`, e.g. `v1.2.3-beta.1`). With no version, output goes to a
  `development/` folder.
- Each release folder holds one subfolder **per platform** plus an archive per
  platform:

```
release/
├── development/
│   └── win32-x64/ …
└── v1.2.3/
    ├── win32-x64/                       (unpacked bundle)
    │   ├── hull-host.exe
    │   ├── libssl-3-x64.dll
    │   ├── libcrypto-3-x64.dll
    │   ├── icon.png                     (window icon — only if window.icon is set)
    │   ├── app.html                     (your whole UI, inlined)
    │   └── <App>.cmd                    (double-click launcher)
    └── App-v1.2.3-win32-x64.zip         (the same, zipped)
```

- **Archive format follows the target**: `.zip` for Windows, `.tar.gz` for macOS
  and Linux. Each archive contains a single top-level folder so it extracts cleanly.
- **Minimal set only**: the host binary, the libraries it actually needs, `app.html`,
  and a launcher. (`webview.dll` is excluded — the host doesn't import it.)
- **macOS emits a `.app` bundle** instead of loose files: `<App>.app/Contents/{Info.plist,
  MacOS/<host + launcher>, Resources/<app.html + icon.icns>}`, archived as `.tar.gz` and
  double-clickable in Finder with its icon. The host links Homebrew OpenSSL by path, so
  the `.app` runs on machines with those libs (the build machine); bundling/relinking the
  dylibs + code-signing & notarizing for other Macs is a later step (see Signing).
- **Window icon**: if `.hullrc` sets `window.icon` (or top-level `icon`), the icon is copied into the bundle
  as `icon.<ext>` and the launcher passes `--icon`. With no icon set, the host falls
  back to the bundled Hull logo (no extra file shipped).
- **Secure build**: with `secure: true` in `.hullrc`, the bundle dir and archive get a
  `-secure` suffix (e.g. `win32-x64-secure/`, `App-v1.2.3-win32-x64-secure.zip`) and
  ship `hull-host-secure`.
- Run a local build with `hull start [version]`.

Unpack the archive on a target machine and run the launcher — it works out of the
box, given the OS web view runtime (WebView2 on Windows 11, WebKitGTK on Linux).

### Native installers

`hull installer [version]` wraps the build for the **current** OS into a native installer
(run `hull build` first):

```bash
hull build && hull installer        # -> release/<version>/<App>-<version>-<key>.<dmg|deb|exe>
```

| OS | Output | How it's built | Install |
|----|--------|----------------|---------|
| macOS | `.dmg` (the `.app` + an Applications drop-link) | `hdiutil` (built in) | open the dmg, drag to Applications |
| Linux | `.deb` (payload in `/opt/<pkg>`, a `/usr/bin` launcher, a `.desktop` + hicolor icon, `Depends:` computed by `dpkg-shlibdeps`) | `dpkg-deb` (built in) | `sudo apt install ./<app>.deb` |
| Windows | `.exe` (per-user install to `{localappdata}\Programs`, Start-Menu + Desktop shortcuts w/ icon, uninstaller) | [Inno Setup](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`) | run the `.exe` |

Each installer is built on its own OS (the packaging tools are OS-native), like the host
binaries. The installers are **unsigned** — fine for your own machines/testing; for public
distribution add code-signing per platform (see Signing below): macOS apps need
signing **+ notarization** (and the `.app`'s Homebrew OpenSSL dylibs bundled/relinked),
Windows installers need Authenticode to avoid SmartScreen warnings.

**Store metadata.** The `.deb` ships an AppStream MetaInfo file
(`/usr/share/metainfo/<appId>.metainfo.xml`) so GNOME Software / App Center shows the
name, summary, icon, **license**, **developer**, and **version/date** instead of
"Unknown". These come from your `package.json` (`description`, `license`, `author`) or
`.hullrc` (`license`, `author`, `description`), and the version is the build's version
(use `hull build vX.Y.Z` for a real number — a `development` build is `0.0.0`).
App Center still labels a manually-installed `.deb` "Potentially unsafe / third party" —
that's about provenance, not the package; only hosting it in a **signed APT repo** (or a
Flatpak/Snap store) removes it.

> **Dev vs installed (Linux):** running an app from a dev build writes a user-level
> `~/.local/share/applications/<appId>.desktop` (hidden, just for the window↔icon match).
> When the **installed** binary runs (from `/opt` or `/usr`), the host detects that and
> skips/removes the user-level entry so the package's visible menu entry isn't shadowed.

### Building bundles for other platforms

`hull build` packages the **current** platform by default. To package others you
need their host binary present:

```bash
npx hull build v1.2.3 --platform all        # every platform whose host is installed
npx hull build v1.2.3 --platform linux-x64  # one specific target
```

> When passing **flags** like `--platform` through `npm run`, the `--` separator is
> required: `npm run build -- --platform all` forwards the flag, but
> `npm run build --platform all` (no `--`) silently drops it. `npx hull …` avoids
> the footgun entirely.

Because the host packages are os/cpu-gated, an app developer normally only has their
own platform's host installed — so the realistic way to produce **all** platforms for
a public release is CI (below), where each OS runner builds its bundle.

### Signing

- **Windows**: Authenticode-sign `hull-host.exe` so SmartScreen stays quiet.
- **macOS**: code-sign + notarize.
- **Linux**: no signing requirement.

---

## Building the host binaries (maintainers)

The host must be built **on its own OS** — true cross-compilation isn't realistic for
WebView2 (Windows) or WebKit (macOS). The one exception is **Linux, via Docker**, which
works from any OS.

| Script | Builds | Where it works |
|--------|--------|----------------|
| `npm run build:host` | current platform (native) | any |
| `npm run build:windows` | `win32-x64` | Windows only |
| `npm run build:mac` | `darwin-<arch>` | macOS only |
| `npm run build:linux` | `linux-x64` | **any OS with Docker**, or native Linux |
| `npm run build:hosts` | everything reachable from this machine | builds current + Linux-via-Docker; reports the rest |

Each stages its binary into `packages/hull-<os>-<arch>/bin` (git-ignored; built
fresh in CI). `build:mac`/`build:windows` print an actionable error when run on the
wrong OS. Honored env: `VCPKG_ROOT`, `OPENSSL_ROOT_DIR`, `GIT_EXECUTABLE`.

### Linux via Docker

`build:linux` uses [`packages/hull/host/linux.Dockerfile`](../packages/hull/host/linux.Dockerfile)
(Ubuntu 24.04 + WebKitGTK 6 / GTK4 / OpenSSL / libsecret / CUPS). It builds the image
once, then compiles the mounted host sources and stages the binary — no Linux machine
required. Needs Docker running.

### Per-platform build deps

- **Windows**: MSVC Build Tools + Windows SDK, CMake, OpenSSL via vcpkg, WebView2 SDK
  (fetched by CMake).
- **macOS**: Xcode CLT, CMake, `brew install openssl@3` (set `OPENSSL_ROOT_DIR`).
- **Linux**: `build-essential cmake git pkg-config libssl-dev libsecret-1-dev
  libcups2-dev libgtk-4-dev libwebkitgtk-6.0-dev` — and `libsqlcipher-dev` for the
  **secure** build (`-DHULL_CRYPTO=ON`). The canonical, exhaustive list (incl. the
  transitive deps a `--no-install-recommends` image needs) is
  [`linux.Dockerfile`](../packages/hull/host/linux.Dockerfile).

See [platforms.md](platforms.md) for caveats.

---

## Publishing (maintainers)

Hosts are delivered as **os/cpu-gated optional dependencies**, so
`npm install @mwguerra/hull` pulls only the binary for the user's machine (the
esbuild model):

```
@mwguerra/hull             CLI + bridge + adapters
@mwguerra/hull-win32-x64   { "os": ["win32"], "cpu": ["x64"] }   prebuilt host + DLLs
@mwguerra/hull-darwin-arm64 / -linux-x64
```

> **Repo vs published manifest.** The platform packages are intentionally **not**
> listed in the repo's `@mwguerra/hull` manifest, because npm validates workspace
> `os`/`cpu` and would reject non-matching platforms during local install. They are
> injected as `optionalDependencies` at publish time by
> [`scripts/prepare-publish.mjs`](../scripts/prepare-publish.mjs). At runtime the CLI
> resolves the host by path (sibling `@mwguerra/hull-<key>`), which works the same in
> the monorepo and in an installed `@mwguerra/` scope.

### CI matrix

[`.github/workflows/release.yml`](../.github/workflows/release.yml) builds the host on
`windows-latest`, `macos-14` (arm64), `macos-13` (x64), and `ubuntu-latest`, uploads
each binary as an artifact, then a publish job re-hydrates them, runs
`prepare-publish`, and `npm publish`es every package. Trigger by pushing a `vX.Y.Z`
tag (needs an `NPM_TOKEN` secret).

> Keep the host and platform packages on the **same version** so the optional-dep
> range always resolves. The host's bridge contract (binding names + JSON shapes) is
> the compatibility surface — treat changes to it as semver-significant.
