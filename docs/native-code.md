# Custom native code (eject)

The prebuilt host covers most apps with its standard bindings (HTTP, storage,
keychain, printing). When you need **app-specific** native code, eject the C++
project and build your own host.

```bash
hull eject     # copies the host into ./desktop
```

This requires a C++ toolchain: **CMake**, a C++17 compiler, and **OpenSSL** (vcpkg on
Windows, `brew install openssl@3` on macOS, `libssl-dev` on Linux — see the per-platform
build steps in the [README](../README.md#build-dependencies-per-platform) and
[platforms.md](platforms.md)).

## What you get

```
desktop/
├── CMakeLists.txt          # FetchContent: webview, json, cpp-httplib; OpenSSL; option(HULL_CRYPTO)
├── README.md
├── third_party/sqlite/     # vendored SQLite amalgamation
└── src/
    ├── main.cpp            # arg parsing, window/serve modes, binding registration
    ├── dispatcher.hpp      # transport-agnostic handler registry (args -> reply) + emit + trace
    ├── serve.hpp           # HTTP/SSE bridge (browser dev mode)
    ├── secure.hpp          # at-rest crypto layer (NullCipher default; AES under -DHULL_CRYPTO)
    ├── paths.hpp / keychain.hpp / db_core.hpp / file_store.hpp   # webview-free cores
    └── bindings/
        ├── http.hpp        ├── storage.hpp     ├── database.hpp
        ├── credentials.hpp ├── printer.hpp     └── files.hpp
```

Bindings register **handlers** on a `Dispatcher` (`d.on(name, handler)`), which is then
exposed over the web view (native) and over HTTP/SSE (browser dev). A handler is
`(const json& args, Reply reply)`; call `reply(resultJson)` exactly once — synchronously
or later from a worker thread.

## Add a synchronous binding

Register it in a `register_*` function (or `register_all` in `main.cpp`). The handler
receives the parsed JSON args and calls `reply` with the result:

```cpp
d.on("greet", [](const json& args, Reply reply) {
  std::string name = args.at(0).get<std::string>();    // args is the JSON arg array
  reply(json{{"ok", true}, {"msg", "Hi " + name}});
});
```

From the UI (works in native and browser dev mode alike):

```js
import { bridge } from "@mwguerra/hull/bridge";
const res = await bridge.invoke("greet", "Marcelo");   // { ok: true, msg: "Hi Marcelo" }
```

## Add an asynchronous binding (for I/O)

Anything that does I/O must run off the UI thread: spawn a worker, then call `reply`
later (see `bindings/http.hpp` / `database.hpp` for full examples):

```cpp
d.on("slowThing", [](const json& args, Reply reply) {
  std::thread([args, reply]() {
    json out;
    try {
      // ... blocking work (network, disk, device) ...
      out = {{"ok", true}};
    } catch (const std::exception& e) {
      out = {{"ok", false}, {"error", e.what()}};
    }
    reply(out);                                         // safe from any thread
  }).detach();
});
```

## Push events to the UI

Capture the dispatcher and call `emit()` to notify the UI with no polling (this also
shows up in the inspector's event stream):

```cpp
d.on("startSync", [&d](const json&, Reply reply) {
  // ... do work ...
  d.emit("sync:done", {{"count", 42}});                // C++ -> UI
  reply(json{{"ok", true}});
});
```

```js
bridge.on("sync:done", ({ count }) => console.log("synced", count));
```

> Never `emit` a secret — pushes are for non-secret app state only.

## Build and run your host

```bash
cd desktop
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
# Windows: -DGIT_EXECUTABLE="C:\Program Files\Git\cmd\git.exe" if FetchContent errors

# Run your host directly against the dev server or a built bundle:
build/bin/Release/hull-host.exe --url http://localhost:5173 --title "My App"
build/bin/Release/hull-host.exe --app ../release/app.html --title "My App"
```

> In the current MVP the `hull` CLI always launches the **prebuilt** host. To use
> your ejected host during `dev`/`start`, run it directly as above, or replace the
> binary in `@mwguerra/hull-<platform>/bin`. A `host` config override to point the
> CLI at a custom binary is on the roadmap.

## When to eject vs. not

- **Don't eject** for HTTP, settings, secrets, printing, SQLite, or file storage —
  those ship in the host.
- **Eject** for: talking to custom hardware/SDKs, heavy native compute, custom SQL in
  C++, or any logic that must live in the backend.
