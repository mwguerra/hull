# Hull documentation

Tiny native desktop apps from your Vue/React/JS UI, powered by a prebuilt C++
web-view host. `npm install` and go — no compiler, no Electron.

## Contents

- [Getting started](getting-started.md) — install, scripts, zero-config, configuration
- [Configuration](configuration.md) — `.hullrc`, defaults vs project overrides, the `secure` flag
- [Architecture](architecture.md) — process model, the JSON bridge, the build pipeline
- [Features](features.md) — every binding with copy-paste examples and what the backend does
- [Dev tools](devtools.md) — browser dev mode (HMR, no recompile) + the inspector window
- [Database](database.md) — embedded SQLite: query/exec/batch/migrate, security, performance
- [Platforms](platforms.md) — Windows vs macOS vs Linux: web view, keychain, printing, build deps, caveats
- [Native code](native-code.md) — `eject` and writing your own C++ bindings
- [Security](security.md) — secrets, TLS, encryption at rest, storage locations
- [Distribution](distribution.md) — how prebuilt hosts are built and published; shipping your app

## Examples

Three runnable apps, each exercising **all** features (bridge, settings + events, credentials, HTTP, printing, SQLite, files):

| Example | Stack | Path |
|---------|-------|------|
| Vanilla JS | Vite, no framework | [`examples/vanilla-js`](../examples/vanilla-js) |
| React | Vite + React | [`examples/react`](../examples/react) |
| Vue | Vite + Vue 3 | [`examples/vue`](../examples/vue) |

```bash
npm install            # link workspaces
npm run build:host     # compile the native host (needs CMake + MSVC + OpenSSL/vcpkg)

npm -w hull-example-vue     run dev     # live dev (HMR) in a native window
npm -w hull-example-react   run build   # package -> examples/react/release
npm -w hull-example-vanilla run start   # run the packaged app
```
