# Hull host (C++)

This is the native runtime that renders your web UI in an OS web view (WebView2 /
WebKit / WebKitGTK) and exposes the JSON bridge. The published packages ship this
**prebuilt** — you only need this source if you ran `hull eject` to add custom
native bindings.

## Layout

```
host/
├── CMakeLists.txt          # FetchContent: webview, json, cpp-httplib; OpenSSL; option(HULL_CRYPTO)
├── third_party/sqlite/     # vendored SQLite amalgamation (default build)
└── src/
    ├── main.cpp            # arg parsing (--url/--app/--serve/--inspect/...), window + serve modes
    ├── dispatcher.hpp      # transport-agnostic handler registry (args -> reply) + emit + trace
    ├── serve.hpp           # HTTP/SSE bridge server (browser dev mode)
    ├── paths.hpp           # app identity + per-user data dir + 0600 (webview-free)
    ├── keychain.hpp        # OS keychain core (webview-free)
    ├── secure.hpp          # at-rest crypto LAYER: NullCipher default, AES under -DHULL_CRYPTO
    ├── db_core.hpp         # SQLite core (vendored, or SQLCipher in the secure build)
    ├── file_store.hpp      # file/upload storage core (through secure.hpp)
    └── bindings/
        ├── http.hpp        # httpPost / httpGet (TLS, keychain-injected auth)
        ├── storage.hpp     # settings store (through the secure layer)
        ├── credentials.hpp # keychain bindings, write-only from the UI
        ├── database.hpp    # dbExec / dbQuery / dbGet / dbBatch
        ├── files.hpp       # fileWrite / fileRead / fileList / fileDelete
        └── printer.hpp     # Winspool / CUPS discovery, ESC/POS, port-9100
```

> Build the **secure** variant (AES files/settings + SQLCipher DB) with
> `-DHULL_CRYPTO=ON` — it produces `hull-host-secure` and requires SQLCipher
> (Linux `libsqlcipher-dev`, macOS `brew install sqlcipher`, Windows `vcpkg install sqlcipher`).

## Add a binding

Bindings register handlers on the `Dispatcher` (`d.on(name, handler)`); they're then
exposed over the web view and over HTTP/SSE automatically. A handler gets the parsed
JSON args and calls `reply` once (sync or async):

```cpp
d.on("myThing", [](const json& args, Reply reply) {
  reply(json{{"ok", true}});                          // resolves the JS Promise
});
```

For anything that does I/O, spawn a worker and call `reply` later (see
`bindings/http.hpp` / `database.hpp`) so the UI thread never blocks. See
[../../../docs/native-code.md](../../../docs/native-code.md) for the full guide.

## Build

Prerequisites: CMake, a C++17 toolchain, and OpenSSL via
[vcpkg](https://github.com/microsoft/vcpkg).

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

The binary is `build/bin/Release/hull-host.exe` (Windows) or `build/bin/hull-host`.

> On Windows, if CMake's FetchContent fails with `ambiguous argument 'HEAD0'`,
> point it at a real Git: add `-DGIT_EXECUTABLE="C:\Program Files\Git\cmd\git.exe"`.

## Runtime flags

| Flag | Meaning |
|------|---------|
| `--url <url>` | load a dev server (HMR) |
| `--app <file.html>` | load a built single-file bundle |
| `--serve <port>` | headless HTTP/SSE bridge (browser dev mode, no window) |
| `--inspect` | enable the dev trace (mirrors calls/events on `__trace`) |
| `--inspect-port <port>` | (window mode) also run a trace server for the inspector |
| `--title <s>` | window title |
| `--width <n>` / `--height <n>` | window size |
| `--icon <path>` | window icon (PNG/ICO; applied on Windows via GDI+, ignored on macOS/Linux) |
| `--app-id <s>` | namespaces storage + keychain per app |
| `--debug` | open dev tools |

## Environment (Linux sandbox)

WebKitGTK sandboxes its subprocesses with bubblewrap (needs unprivileged user
namespaces). When those are blocked (Ubuntu 24.04 default, many containers) the host
**auto-detects** it and disables the sandbox so the app runs. Override:

| Env var | Effect |
|---------|--------|
| `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` | disable the sandbox (respected; skips the probe) |
| `HULL_FORCE_SANDBOX=1` | keep the sandbox, never auto-disable |

The CLI sets these from `.hullrc` `linux.sandbox` / `hull … --no-sandbox`.
