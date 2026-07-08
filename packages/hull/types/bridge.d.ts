// Type definitions for @mwguerra/hull/bridge
// Hand-maintained alongside src/bridge/index.js — every export there is typed here.

export type BridgeMode = "native" | "http" | "none";

export interface OkResult {
  ok: boolean;
  error?: string | null;
}

// ---------------------------------------------------------------- core ------

export interface Bridge {
  /** Transport: "native" (host web view) | "http" (browser dev) | "none". */
  readonly mode: BridgeMode;
  /** Low-level call: invoke a host binding by name. */
  invoke(name: string, ...args: unknown[]): Promise<any>;
  /** Subscribe to a C++ -> UI event. Returns an unsubscribe function. */
  on(event: string, handler: (payload: any) => void): () => void;
  off(event: string, handler: (payload: any) => void): void;
}

export const bridge: Bridge;

export function bridgeMode(): BridgeMode;
/** True when the backend is reachable (native host OR browser dev mode). */
export function hasBridge(): boolean;
/** True only inside the native host web view. */
export function isNative(): boolean;

export function ping(text?: unknown): Promise<OkResult & { echo: unknown }>;
export function appInfo(): Promise<OkResult & { appId: string; secure: boolean }>;

// ---------------------------------------------------------------- settings --

export function saveSetting(key: string, value: unknown): Promise<OkResult>;
export function loadSetting(key: string): Promise<OkResult & { value: unknown }>;
export function loadAllSettings(): Promise<OkResult & { value: Record<string, unknown> }>;

export interface NativeSettingStore<T = unknown> {
  get(): T | undefined;
  load(): Promise<T | undefined>;
  set(value: T): Promise<void>;
  subscribe(handler: (value: T) => void): () => void;
}
export function nativeSetting<T = unknown>(key: string): NativeSettingStore<T>;

// ---------------------------------------------------------------- secrets ---

export function saveCredential(service: string, account: string, secret: string): Promise<OkResult>;
export function credentialExists(service: string, account: string): Promise<OkResult & { exists: boolean }>;
export function eraseCredential(service: string, account: string): Promise<OkResult>;

// ---------------------------------------------------------------- http ------

export interface HttpResponse {
  ok: boolean;
  status?: number;
  headers?: Record<string, string>;
  /** Parsed JSON when the body is JSON, else the raw string. */
  body?: any;
  error?: string | null;
}

export interface HttpRequestOptions {
  url: string;
  method?: "GET" | "POST" | "PUT" | "PATCH" | "DELETE" | "HEAD" | "OPTIONS";
  /** Custom request headers. */
  headers?: Record<string, string>;
  /** object/array -> sent as application/json; string -> raw (set a Content-Type header). */
  body?: unknown;
  /** multipart/form-data parts (POST only). */
  form?: Array<
    | { name: string; value: string }
    | { name: string; fileName: string; contentBase64: string; contentType?: string }
  >;
  timeoutMs?: number;
  /** false disables the keychain Bearer-token injection. Default true. */
  auth?: boolean;
  followRedirects?: boolean;
}

export function httpPost(url: string, body: unknown): Promise<HttpResponse>;
export function httpGet(url: string): Promise<HttpResponse>;

export interface HttpDownloadOptions {
  url: string;
  /** Absolute destination path (usually from dialogs.save). */
  path: string;
  headers?: Record<string, string>;
  timeoutMs?: number;
  auth?: boolean;
}

export const http: {
  request(options: HttpRequestOptions): Promise<HttpResponse>;
  get(url: string, options?: Partial<HttpRequestOptions>): Promise<HttpResponse>;
  post(url: string, body?: unknown, options?: Partial<HttpRequestOptions>): Promise<HttpResponse>;
  put(url: string, body?: unknown, options?: Partial<HttpRequestOptions>): Promise<HttpResponse>;
  patch(url: string, body?: unknown, options?: Partial<HttpRequestOptions>): Promise<HttpResponse>;
  delete(url: string, options?: Partial<HttpRequestOptions>): Promise<HttpResponse>;
  /**
   * Stream a URL to disk. Progress arrives as bridge events:
   * `bridge.on("http:download", ({ url, path, received, total }) => ...)`.
   */
  download(options: HttpDownloadOptions): Promise<OkResult & { path?: string; bytes?: number }>;
};

// ---------------------------------------------------------------- updates ---

export interface UpdateManifest {
  version: string;
  generatedAt?: string;
  notes?: string;
  platforms?: Record<string, { file: string; bytes: number; url?: string }>;
  [key: string]: unknown;
}

/** Semver-ish compare: 1 if a > b, -1 if a < b, 0 if equal (prereleases rank low). */
export function compareVersions(a: string, b: string): number;

export const updates: {
  check(manifestUrl: string, currentVersion: string): Promise<
    OkResult & { updateAvailable?: boolean; latest?: UpdateManifest; currentVersion?: string }>;
  download(url: string, path: string, options?: Partial<HttpDownloadOptions>):
    Promise<OkResult & { path?: string; bytes?: number }>;
  openInstaller(path: string): Promise<OkResult>;
};

// ---------------------------------------------------------------- theme -----

export type Theme = "dark" | "light";
/** Current OS theme (via prefers-color-scheme — the web views follow the OS). */
export function getTheme(): Theme;
/** Calls cb on every OS theme change. Returns an unsubscribe function. */
export function onThemeChanged(cb: (theme: Theme) => void): () => void;

// ---------------------------------------------------------------- window ----

export function setFullscreen(on?: boolean): Promise<OkResult & { fullscreen?: boolean }>;
export function isFullscreen(): Promise<OkResult & { fullscreen?: boolean }>;
export function toggleFullscreen(): Promise<OkResult & { fullscreen?: boolean }>;

export interface WindowBounds extends OkResult {
  /** Absent on Linux/Wayland (the compositor owns placement). */
  x?: number;
  y?: number;
  /** Present when ok; absent on the { ok: false } error path. */
  width?: number;
  height?: number;
  maximized?: boolean;
  fullscreen?: boolean;
}

/**
 * Native window control (host only). center/setAlwaysOnTop/setPosition are
 * unsupported on Linux/Wayland and reply { ok: false } there.
 */
export const appWindow: {
  setFullscreen(on?: boolean): Promise<OkResult & { fullscreen?: boolean }>;
  isFullscreen(): Promise<OkResult & { fullscreen?: boolean }>;
  toggleFullscreen(): Promise<OkResult & { fullscreen?: boolean }>;
  minimize(): Promise<OkResult>;
  maximize(on?: boolean): Promise<OkResult>;
  isMaximized(): Promise<OkResult & { maximized?: boolean }>;
  show(): Promise<OkResult>;
  hide(): Promise<OkResult>;
  center(): Promise<OkResult>;
  setAlwaysOnTop(on?: boolean): Promise<OkResult>;
  setSize(width: number, height: number): Promise<OkResult>;
  setPosition(x: number, y: number): Promise<OkResult>;
  getBounds(): Promise<WindowBounds>;
};

// ---------------------------------------------------------------- links -----

/** Open a URL with the OS default handler (http/https/mailto/tel; fails closed). */
export function openExternal(url: string): Promise<OkResult & { url?: string }>;
/** Open web content in a NEW Hull window (plain web view, no bridge bindings). */
export function openWindow(url: string, options?: {
  title?: string; width?: number; height?: number;
}): Promise<OkResult & { url?: string }>;

// ---------------------------------------------------------------- shell -----

/** Open a local file/folder with its OS default application. */
export function openPath(path: string): Promise<OkResult>;
/** Reveal a file in the OS file manager, selected. */
export function revealPath(path: string): Promise<OkResult>;
/** Move a file/folder to the OS trash / recycle bin. */
export function trashPath(path: string): Promise<OkResult>;

// ---------------------------------------------------------------- dialogs ---

export interface FileFilter {
  name: string;
  /** Extensions without dots, e.g. ["png", "jpg"]. */
  extensions: string[];
}

export const dialogs: {
  open(options?: {
    title?: string;
    /** Pick folder(s) instead of file(s). */
    directory?: boolean;
    multiple?: boolean;
    filters?: FileFilter[];
  }): Promise<OkResult & { canceled: boolean; paths: string[] }>;
  save(options?: {
    title?: string;
    defaultName?: string;
    filters?: FileFilter[];
  }): Promise<OkResult & { canceled: boolean; path: string | null }>;
  /** Windows maps buttons to its fixed OK/Cancel/Yes/No sets (count-based). */
  message(options: {
    message: string;
    title?: string;
    detail?: string;
    buttons?: string[];
    type?: "info" | "warning" | "error";
    defaultId?: number;
  }): Promise<OkResult & { button?: number }>;
};

// ---------------------------------------------------------------- clipboard -

export const clipboard: {
  /** text is null when the clipboard is empty or holds no text. */
  readText(): Promise<OkResult & { text?: string | null }>;
  writeText(text: string): Promise<OkResult>;
};

// ---------------------------------------------------------------- tray ------

export interface TrayMenuItem {
  id: string;
  label: string;
  type?: "normal" | "separator" | "checkbox";
  checked?: boolean;
  enabled?: boolean;
}

/**
 * Windows + macOS (Linux replies { ok: false } — no StatusNotifierItem host).
 * Events: bridge.on("tray:menu", ({ id }) => ...) and (Windows) "tray:click".
 */
export const tray: {
  set(options?: { tooltip?: string; icon?: string; menu?: TrayMenuItem[] }): Promise<OkResult>;
  remove(): Promise<OkResult>;
};

// ---------------------------------------------------------------- notify ----

/**
 * System notification. Title falls back to the app title (native). Clicks emit
 * the "notification:clicked" bridge event.
 */
export function notify(title: string, body?: string): Promise<OkResult>;

// ---------------------------------------------------------------- database --

export interface DbExecResult {
  changes: number;
  lastInsertRowid: number;
}

export interface DbTransactionBuilder {
  /** Queue a statement; the whole batch runs as one atomic transaction. */
  exec(sql: string, params?: unknown[]): void;
}

export const db: {
  exec(sql: string, params?: unknown[]): Promise<DbExecResult>;
  query<T = Record<string, unknown>>(sql: string, params?: unknown[]): Promise<T[]>;
  get<T = Record<string, unknown>>(sql: string, params?: unknown[]): Promise<T | null>;
  batch(statements: Array<{ sql: string; params?: unknown[] }>): Promise<unknown[]>;
  /** Batch sugar: queue statements synchronously, run atomically. */
  transaction(build: (tx: DbTransactionBuilder) => void): Promise<unknown[]>;
  /** Online snapshot (VACUUM INTO). Fails if the destination exists. */
  backup(path: string): Promise<string>;
  /** Ordered, run-once migrations tracked via PRAGMA user_version. */
  migrate(steps: Array<string | { sql: string }>): Promise<number>;
};

// ---------------------------------------------------------------- files -----

export const files: {
  /** Managed per-user store (namespaced by appId; through the secure layer). */
  write(name: string, content: string | Uint8Array | ArrayBuffer | Blob): Promise<void>;
  read(name: string): Promise<Uint8Array>;
  readText(name: string): Promise<string>;
  list(): Promise<Array<{ name: string; size: number }>>;
  remove(name: string): Promise<boolean>;
  /** Path-based IO for user-picked absolute paths (see dialogs.open/save). */
  readAt(path: string): Promise<Uint8Array>;
  readTextAt(path: string): Promise<string>;
  writeAt(path: string, content: string | Uint8Array | ArrayBuffer | Blob): Promise<void>;
};

// ---------------------------------------------------------------- printing --

export function listPrinters(): Promise<OkResult & { printers?: Array<{ name: string; isDefault?: boolean }> }>;
export function printMessage(printer: string, text: string): Promise<OkResult>;
export function printReceipt(printer: string, text: string): Promise<OkResult>;
export function printNetwork(host: string, port: number, text: string): Promise<OkResult>;
