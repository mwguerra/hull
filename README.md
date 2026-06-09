# Hull

Tiny native desktop apps from your web UI, powered by a prebuilt C++ web-view
host. `npm install` and go — no compiler, no Electron.

This is the monorepo. The published package is **[`@mwguerra/hull`](packages/hull/README.md)**.
Full docs live in **[`docs/`](docs/README.md)**.

## Packages

| Package | What it is |
|---------|------------|
| [`@mwguerra/hull`](packages/hull) | The CLI (`dev`/`build`/`start`/`installer`/`eject`), the JS bridge runtime, and the Vue/React adapters. Also carries the C++ host **sources** (for `eject`). |
| `@mwguerra/hull-win32-x64` | Prebuilt host binary for Windows x64 (os/cpu-gated optional dep). One per platform; CI builds the rest. |

## Examples

Three runnable apps, each exercising **all** features (bridge, settings + events, credentials, HTTP, printing, SQLite, files):

| Example | Stack | Path |
|---------|-------|------|
| Vanilla JS | Vite, no framework | [`examples/vanilla-js`](examples/vanilla-js) |
| React | Vite + React | [`examples/react`](examples/react) |
| Vue | Vite + Vue 3 | [`examples/vue`](examples/vue) |

## Try it

```bash
npm install                 # links workspaces
npm run build:host          # compile the native host -> packages/hull-win32-x64/bin
                            #   (needs CMake + MSVC + OpenSSL via vcpkg)

npm -w hull-example-vue run dev               # live dev (HMR) in a native window + inspector tab
npm -w hull-example-vue run dev -- --browser  # develop in your browser (no recompile) + inspector
npm -w hull-example-react run build           # bundle the UI + package with the host
npm -w hull-example-vanilla run start         # launch a packaged app
```

Add `-v` to any command for per-step timings; every command prints its total time.

## Documentation

[Getting started](docs/getting-started.md) ·
[Configuration](docs/configuration.md) ·
[Architecture](docs/architecture.md) ·
[Features](docs/features.md) ·
[Dev tools](docs/devtools.md) ·
[Database](docs/database.md) ·
[Platforms](docs/platforms.md) ·
[Native code](docs/native-code.md) ·
[Security](docs/security.md) ·
[Distribution](docs/distribution.md)

## Architecture

```
your Vue/React app  ──Vite single-file build──▶  one app.html
                                                     │
                          ┌──────────────────────────┘
                          ▼
   prebuilt hull-host (C++)  ──renders in OS web view──▶  native window
        │  JSON bridge (UI ⇄ C++)
        ├─ HTTP (TLS)         ├─ settings + files (secure layer; crypto opt-in)
        ├─ OS keychain        ├─ printing (Winspool / CUPS / port-9100)
        └─ SQLite (parameterized, per-user; SQLCipher in the secure build)
```

The host is **generic and prebuilt**: one binary runs any app, parameterized by
CLI flags (`--url` for dev, `--app` for prod, `--app-id` to namespace storage).
This is what makes "npm install and go" possible — app developers never touch C++.

For app-specific native code, `hull eject` drops the C++ project so you can add
bindings and compile your own host.

## Status

- **All three platforms tested** — Windows x64, macOS (Apple Silicon), and Linux x64:
  host builds, the window opens, the JS bridge works, and `dev` / `build` / `start` /
  `installer` all run. Packaged Windows app ~8 MB unpacked (~3.5 MB zipped).
- **Examples** — vanilla JS, React, Vue, each exercising all features (bridge, settings + events, credentials, HTTP, printing, SQLite, files, image upload); all build + launch.
- **Releases** — versioned (`vX.Y.Z`/`development`), per-platform archives (zip/tar.gz) + native **installers** (`.dmg` / `.deb` / `.exe`).
- **Capabilities** — HTTP/TLS, encrypted-at-rest (opt-in secure build: AES + SQLCipher), OS keychain, printing (documents + ESC/POS), **SQLite**, file storage, two-way state.
- **DX** — `-v` timings on all commands; **browser dev mode** (`hull dev --browser`, full HMR, no recompile) + a dev-only **inspector** window (stripped from production).
- **Not yet** — code-signing / notarization (installers are unsigned); macOS is unsigned + the `.app`'s OpenSSL dylibs aren't bundled for *other* Macs yet.

## Building the host binaries

A host must be built on its own OS — true cross-compile isn't realistic for WebView2
(Windows) or WebKit (macOS). **Linux is the exception: it builds from any OS via Docker.**

| Script | Builds | Where |
|--------|--------|-------|
| `npm run build:host` | current platform | any |
| `npm run build:windows` | `win32-x64` | Windows only |
| `npm run build:mac` | `darwin-<arch>` | macOS only |
| `npm run build:linux` | `linux-x64` | **any OS with Docker**, or native Linux |
| `npm run build:hosts` | everything reachable here | current + Linux-via-Docker; reports the rest |

### Build dependencies (per platform)

Each host is built from source the first time (CMake fetches webview / json / cpp-httplib).
Install the toolchain for the OS you're building on, then run the script above.

**macOS** (builds `darwin-arm64`, Apple Silicon only):

```bash
xcode-select --install                 # clang, make, git
brew install cmake openssl@3 node      # + sqlcipher  (only for the secure build)
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"   # Homebrew OpenSSL isn't on CMake's default path
npm run build:host                     # or build:host:secure
```
WebKit, CUPS, and the Security/CoreFoundation frameworks ship with macOS — nothing to install.

**Windows** (`win32-x64`):

```powershell
# Visual Studio 2022 Build Tools (C++ workload + Windows 11 SDK), CMake, and Git for Windows.
winget install Microsoft.VisualStudio.2022.BuildTools Kitware.CMake Git.Git
# OpenSSL via vcpkg:
git clone https://github.com/microsoft/vcpkg "$env:USERPROFILE\vcpkg"; & "$env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat"
& "$env:USERPROFILE\vcpkg\vcpkg.exe" install openssl:x64-windows   # + sqlcipher:x64-windows for secure
$env:VCPKG_ROOT="$env:USERPROFILE\vcpkg"; npm run build:host
```
If CMake's FetchContent fails with `ambiguous argument 'HEAD0'`, point it at a real Git:
`$env:GIT_EXECUTABLE="C:\Program Files\Git\cmd\git.exe"`.

**Linux** — Ubuntu 24.04+ (`libwebkitgtk-6.0-dev` needs 24.04 or newer):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config ca-certificates \
  libssl-dev libsecret-1-dev libcups2-dev libgnutls28-dev libavahi-client-dev \
  libgtk-4-dev libwebkitgtk-6.0-dev          # + libsqlcipher-dev for the secure build
npm run build:host                            # or build:host:secure
```
Or build the Linux host **from any OS** with Docker: `npm run build:linux` (uses
[`packages/hull/host/linux.Dockerfile`](packages/hull/host/linux.Dockerfile)).

The honest "all platforms" path is the **CI matrix** in
[`.github/workflows/release.yml`](.github/workflows/release.yml): one runner per OS
builds its host, then a publish job pins `optionalDependencies` (via
[`scripts/prepare-publish.mjs`](scripts/prepare-publish.mjs)) and publishes every
package. See [docs/distribution.md](docs/distribution.md).

## Releasing an app

`hull build [vX.Y.Z]` emits a versioned, self-contained bundle + archive
(`release/<version|development>/<platform>/` + `.zip`/`.tar.gz`) with the minimal
runnable set — unpack on the target and run.

## License

MIT
