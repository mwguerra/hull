# Platforms: Windows vs macOS vs Linux

Hull targets all three desktops from one codebase. The web view, the JSON bridge,
the async model, and the entire Vite/UI toolchain behave identically everywhere.
The native modules (web view backend, keychain, printing, storage paths) compile a
different branch per OS behind `#if defined(_WIN32) / __APPLE__ / else`.

## At a glance

| Area | Windows | macOS | Linux |
|------|---------|-------|-------|
| Web view backend | Edge **WebView2** | **WKWebView** (Cocoa) | **WebKitGTK** |
| Runtime needed by users | WebView2 runtime (ships w/ Win 11) | built in | `libwebkitgtk-6.0` / `libwebkit2gtk-4.1` |
| Keychain | Credential Manager | Keychain | Secret Service (libsecret) |
| Printing | Winspool (RAW) | CUPS | CUPS |
| Notifications | tray balloon → toast/Action Center | Notification Center (`.app`) / osascript (dev) | D-Bus `org.freedesktop.Notifications` |
| Notification clicks | balloon click → `notification:clicked` | activation → `notification:clicked` | D-Bus default action → `notification:clicked` |
| Tray / status item | `Shell_NotifyIcon` + popup menu | `NSStatusBar` + `NSMenu` | **not supported** (`{ ok: false }`) — see below |
| Fullscreen | borderless (monitor bounds) | fullscreen Space (animated) | `gtk_window_fullscreen` |
| Window placement (`center`, `setPosition`, `alwaysOnTop`) | native | native | **unsupported on Wayland** — the compositor owns placement |
| Clipboard | Win32 clipboard (`CF_UNICODETEXT`) | `NSPasteboard` | `GdkClipboard` (async made sync) |
| Dialogs | `IFileOpenDialog` / `MessageBoxW` (fixed button sets) | `NSOpenPanel` / `NSAlert` | `GtkFileDialog` / `GtkAlertDialog` (GTK 4.10+) |
| Open external link | `ShellExecuteW` | `NSWorkspace openURL:` | gio `launch_default_for_uri` |
| Port-9100 raw socket | Winsock (`ws2_32`) | POSIX sockets | POSIX sockets |
| Storage dir | `%LOCALAPPDATA%\<appId>` | `~/Library/Application Support/<appId>` | `$XDG_DATA_HOME/<appId>` or `~/.local/share/<appId>` |
| File permission lockdown | per-user `%LOCALAPPDATA%` | `chmod 0600` / dir `0700` | `chmod 0600` / dir `0700` |
| TLS + encryption (OpenSSL) | vcpkg | `brew install openssl@3` | `libssl-dev` |

`<appId>` comes from your `.hullrc` `appId` (default `com.hull.<name>`),
so each app gets its own storage namespace and keychain entries.

## Runtime requirements (end users)

- **Windows** — the **WebView2 runtime**. Present on Windows 11; on Windows 10,
  ship or require the Evergreen Bootstrapper. The host also needs the OpenSSL DLLs
  (bundled in `release/`) and the VC++ runtime (MSVCP140/VCRUNTIME140 — present on
  most machines; otherwise ship the VC++ Redistributable).
- **macOS** — nothing extra; WKWebView and the Security/CoreFoundation frameworks
  are part of the OS. Code-sign + notarize for distribution.
- **Linux** — the prebuilt host targets **WebKitGTK 6.0**, so it runs on current
  distros: **Ubuntu 24.04+/24.10, Debian 13, Fedora 39+** (glibc ≥ 2.38). It does
  **not** run on Ubuntu 22.04 / Debian 12 or older (older glibc, and no
  WebKitGTK 6.0). The libraries are not bundled — install the runtime set:
  ```bash
  sudo apt install libwebkitgtk-6.0-0   # pulls GTK4; openssl3/libsecret/cups are usually already present
  ```
  A running **Secret Service** provider (gnome-keyring or KWallet) is needed for the
  keychain, and a desktop session for the web view (a headless box has neither —
  expected for a GUI app).

## Keychain caveats

- **macOS keychain APIs are deprecated.** The host uses
  `SecKeychainAddGenericPassword` / `…Find…` which still link and work but emit
  `-Wdeprecated-declarations` and would fail a `-Werror` build. For a warning-clean
  build, port the `__APPLE__` branch to `SecItemAdd` / `SecItemCopyMatching` /
  `SecItemDelete` with `kSecClass = kSecClassGenericPassword`.
- **Linux needs a Secret Service** at runtime (see above).

## Notification caveats

- **macOS uses the deprecated `NSUserNotification` API** (same trade-off as the
  keychain: works everywhere, no bundle-signing requirements that
  `UNUserNotificationCenter` adds). Notification Center requires a bundle id, so
  it's used by packaged `.app`s; the raw dev binary falls back to `osascript`
  (the notification shows, attributed to Script Editor).
- **Windows** balloons need the app window (they attach a tray icon to it), so
  `notify` replies with an error in headless serve mode — browser dev mode uses
  the Web Notification API instead.
- **Linux** requires a notification daemon on the session bus (present on GNOME,
  KDE, and every mainstream desktop).
- **Clicks** work on all three: a Windows balloon click, a macOS notification
  activation, and the Linux D-Bus default action all emit the
  `notification:clicked` bridge event.

## Tray caveats

- **Linux has no tray support** — a modern Linux tray means implementing the
  D-Bus StatusNotifierItem + dbusmenu specs (or depending on libappindicator),
  neither of which fits Hull's zero-extra-deps host. `tray.set` replies
  `{ ok: false }` with that explanation. Windows and macOS are fully supported.
- **`tray:click`** (icon left-click) is Windows-only; on macOS clicking the icon
  opens the menu.

## Window control caveats

- **Linux/Wayland**: the compositor owns window placement, so `appWindow.center()`,
  `setAlwaysOnTop()` and `setPosition()` reply `{ ok: false }` and `getBounds()`
  omits `x`/`y`. Sizing, minimize/maximize, show/hide, fullscreen, and the
  size half of `window.rememberState` all work.

## Printing notes

- macOS and Linux share the CUPS branch; Windows uses Winspool.
- `printMessage` sends a **text document** — GDI on Windows, CUPS `text/plain` on
  macOS/Linux — so it works with **any** printer (Microsoft Print to PDF, OneNote,
  laser printers). Single page, word-wrapped.
- `printReceipt` (local spooler) and `printNetwork` (TCP port 9100) send **raw
  ESC/POS** for thermal receipt printers. For other raw formats, `eject` + a custom
  host binding.

## Build caveats (only relevant if you `eject` or build hosts)

- **OpenSSL is a hard dependency** (HTTPS + encryption) and ships on neither
  Windows nor recent macOS by default:
  - Linux: `libssl-dev`
  - macOS: `brew install openssl@3`, then `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`
  - Windows: `vcpkg install openssl` + the vcpkg toolchain file
- **WebKitGTK version varies by distro.** Hull targets API **6.0**
  (`libwebkitgtk-6.0-dev`); on older systems use `4.1` (`libwebkit2gtk-4.1-dev`)
  and set `WEBVIEW_WEBKITGTK_API` accordingly.
- **String width on Windows.** The host widens identifiers byte-by-byte
  (`std::wstring(s.begin(), s.end())`), correct for ASCII but wrong for non-ASCII
  printer names / credential keys. Convert with `MultiByteToWideChar(CP_UTF8, …)`
  if you need Unicode there.
- **Extra Windows link libs:** `Advapi32` (Credential Manager), `Winspool`
  (printing), `Crypt32`, `ws2_32` (port-9100 socket) — already wired in the host's
  CMake.
- **CMake + Git on Windows:** if FetchContent fails with `ambiguous argument
  'HEAD0'`, you're using a Git shim that pollutes stdout. Pass
  `-DGIT_EXECUTABLE="C:\Program Files\Git\cmd\git.exe"`.

## Troubleshooting

### Linux: `bwrap: setting up uid map: Permission denied`

```
bwrap: setting up uid map: Permission denied
** (process:…): ERROR **: Failed to fully launch dbus-proxy: Child process exited with code 1
```

WebKitGTK sandboxes its subprocesses with **bubblewrap**, which needs **unprivileged
user namespaces**. They're blocked on **Ubuntu 24.04** (the
`kernel.apparmor_restrict_unprivileged_userns=1` default) and in many containers.

Hull's host **auto-detects** this and disables the WebKitGTK sandbox so the app still
runs (with a one-line notice), so a current host usually "just works". If you're on an
older host binary or want to control it:

- **Disable the sandbox** (quickest): `hull start --no-sandbox` / `hull dev --no-sandbox`,
  or `.hullrc` `{ "linux": { "sandbox": false } }`, or env
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`.
- **Keep the sandbox** (preferred for untrusted content) — enable userns instead:
  ```bash
  # Ubuntu 24.04:
  sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
  # older Debian/Ubuntu:
  sudo sysctl -w kernel.unprivileged_userns_clone=1
  ```
  In a container, run with `--security-opt seccomp=unconfined` (or `--cap-add SYS_ADMIN`).
  To force the sandbox on and skip the auto-disable, set `HULL_FORCE_SANDBOX=1` or
  `.hullrc` `{ "linux": { "sandbox": true } }`.

See [configuration.md](configuration.md#the-linuxsandbox-key).

## Verification status

Tested on **Windows x64, macOS (Apple Silicon), and Linux x64**: the host builds, the
window opens, the JS bridge works, and `dev` / `build` / `start` / `installer` run on
each. **Code-signing/notarization is available as opt-in `.hullrc` `sign` config**
(unsigned remains the default — see
[distribution.md](distribution.md#signing)); remaining work is bundling the macOS
`.app`'s OpenSSL dylibs for distribution to *other* Macs, and a Linux tray. See
[distribution.md](distribution.md) for building hosts and installers per platform.
