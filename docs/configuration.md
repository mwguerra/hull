# Configuration (`.hullrc`)

Hull is zero-config: with no config file, **all defaults come from the package**.
To override any of them, add a `.hullrc` (JSON) to your project root. Only the keys
you set are overridden — everything else keeps its default.

```jsonc
// .hullrc
{
  "appId": "com.you.notes",        // namespaces storage + keychain (default: com.hull.<pkg name>)
  "secure": false,                  // use the crypto host build (encrypted at rest)
  "window": {
    "title": "Notes",               // default: package.json productName, else name
    "width": 1100,                  // default: 1100
    "height": 760,                  // default: 760
    "icon": "build/app-icon.png"    // window/app icon (default: bundled Hull logo)
  },
  "debug": false,                   // open dev tools (default: false)
  "outDir": "dist",                 // Vite build output (default: dist)
  "releaseDir": "release",          // packaged output (default: release)
  "linux": { "sandbox": true }      // WebKitGTK sandbox (default: auto — see below)
}
```

## Resolution order

For each key: **`.hullrc` → package defaults**. `window` is merged deeply, so
`{"window": {"title": "X"}}` keeps the default width/height. With no `.hullrc`,
every key uses its default (title/appId derived from `package.json`).

> Hull looks for `.hullrc`, then `.hullrc.json`, then `hull.config.json` (in that
> order) and uses the first one found. `.hullrc` is the recommended name; the others
> are supported as fallbacks.

## Keys

| Key | Default | Meaning |
|-----|---------|---------|
| `appId` | `com.hull.<name>` | per-app namespace for the data dir + keychain entries |
| `secure` | `false` | launch the crypto host build (`hull-host-secure`); see [security.md](security.md) |
| `window.title` | pkg `productName`/`name` | native window title |
| `window.width` / `window.height` | `1100` / `760` | window size |
| `window.icon` (or `icon`) | bundled Hull logo | window/app icon; see [The `window.icon` key](#the-windowicon-key) |
| `debug` | `false` | open the web-view dev tools |
| `outDir` | `dist` | Vite UI build dir |
| `releaseDir` | `release` | packaged app output dir |
| `linux.sandbox` | auto | Linux WebKitGTK sandbox: `true` force on, `false` force off, omit for auto; see [The `linux.sandbox` key](#the-linuxsandbox-key) |
| `license` / `author` / `description` | from `package.json` | installer/store metadata (SPDX license, developer name, summary) for the Linux `.deb` AppStream MetaInfo; see [distribution.md](distribution.md#native-installers) |

## The `window.icon` key

Sets the app's window icon. Point it at a **PNG** relative to your project root —
a square image (e.g. 256×256) works best:

```jsonc
{ "window": { "title": "Notes", "icon": "build/app-icon.png" } }
```

If you omit it (or the file is missing), Hull falls back to the **bundled Hull
logo**, so every app gets a real icon out of the box. You can also set it at the
top level (`"icon": "..."`) — `window.icon` takes precedence.

The icon is applied at runtime by the prebuilt host and is copied next to the
binary when you `hull build`, so packaged apps stay self-contained.

**Platform support:**

- **Windows** — set at runtime via GDI+ → `WM_SETICON`.
- **Linux** — GTK4 can't set a window icon from a file at runtime, so on `hull dev` /
  `start` the host installs **desktop integration** automatically: it copies the icon
  to `~/.local/share/icons/hicolor/256x256/apps/<appId>.png`, writes
  `~/.local/share/applications/<appId>.desktop` (`Icon=<appId>`), and sets the window's
  app-id to `<appId>` so the compositor (GNOME, etc.) shows it. Works on Wayland and
  X11. A freshly-installed icon may take a moment (or a re-login) for the shell to pick
  up. Use a square PNG (256×256).
- **macOS** — `hull build` packages a `.app` bundle and generates its icon (`icon.icns`,
  via `sips`) from `window.icon` (or the bundled Hull logo), so the **packaged app shows
  the icon** in Finder/Dock. There's no runtime file-icon API on macOS, so `hull dev` /
  running the raw binary shows a generic icon — build the `.app` to see it.

SVGs aren't valid native icons; use a PNG.

## The `linux.sandbox` key

On Linux, WebKitGTK isolates its web/network subprocesses with **bubblewrap**, which
needs **unprivileged user namespaces**. Some environments block them — Ubuntu 24.04
enables `kernel.apparmor_restrict_unprivileged_userns` by default, and many containers
lack userns — and the app then aborts at launch with:

```
bwrap: setting up uid map: Permission denied
** (process:…): ERROR **: Failed to fully launch dbus-proxy: Child process exited with code 1
```

By default (`linux.sandbox` omitted) Hull's host **auto-detects** this: it probes
whether a user namespace can be created and, if not, disables the WebKitGTK sandbox so
the app still runs, printing a one-line notice. You usually don't need to set anything.

To override the auto behavior:

```jsonc
{ "linux": { "sandbox": false } }   // always disable the sandbox (no probe)
{ "linux": { "sandbox": true } }    // always keep it (never auto-disable)
```

Equivalents without `.hullrc`:

- `hull dev --no-sandbox` / `hull start --no-sandbox` — disable for that run.
- env `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` — disable (respected by the host).
- env `HULL_FORCE_SANDBOX=1` — keep the sandbox, skip the auto-disable.

These set the same env var the generated Linux launcher uses, so packaged apps behave
the same. To **keep** the sandbox, enable userns instead, e.g.
`sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0` (Ubuntu 24.04) or
`kernel.unprivileged_userns_clone=1` (older Debian/Ubuntu). On macOS/Windows this key
is ignored. See [platforms.md](platforms.md#troubleshooting).

## The `secure` flag

`"secure": true` makes `hull dev` / `build` / `start` use the **crypto** host
(`hull-host-secure`) instead of the default fast host. That build must exist for
your platform (built with `-DHULL_CRYPTO=ON` / `npm run build:host:secure`, or
shipped in the platform package). If it isn't present, the CLI tells you how to build
it. See [security.md](security.md) and [database.md](database.md).
