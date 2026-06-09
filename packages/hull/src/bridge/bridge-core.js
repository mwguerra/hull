// Framework-agnostic transport for the native C++ bridge. Auto-selects:
//   - "native": running inside the host web view (window.<binding> functions exist)
//   - "http":   running in a normal browser with a Hull dev server
//               (window.__HULL_BRIDGE__ = "http://127.0.0.1:<port>", injected by
//                `hull dev --browser`) — invoke over POST, events over SSE
//   - "none":   plain browser, no host (calls reject gracefully)
//
//  invoke(name, ...args) -> Promise  (UI -> C++)
//  on(event, handler)               (C++ -> UI; also "__trace" for the inspector)

function bridgeUrl() {
  return (typeof globalThis !== "undefined" && globalThis.__HULL_BRIDGE__) || null;
}

// The HTTP/SSE transport is for browser DEV mode only. Vite replaces import.meta.env.DEV
// with `false` in `hull build`, so the entire HTTP branch is dead-code-eliminated from
// the shipped app.html — production apps carry only the native transport.
const DEV = import.meta.env.DEV;

class NativeBridge {
  constructor() {
    this._handlers = new Map(); // event -> Set<handler>
    this._url = bridgeUrl();
    this.mode = this._detectMode();

    if (this.mode === "native") {
      // The C++ side pushes events via eval(window.__bridgeEmit(event, jsonString)).
      window.__bridgeEmit = (event, jsonString) => {
        let payload;
        try { payload = JSON.parse(jsonString); } catch { payload = jsonString; }
        this._dispatch(event, payload);
      };
    } else if (DEV && this.mode === "http") {
      // Inlined (not a method) so the whole block is dead-code-eliminated when
      // DEV is false — production app.html carries no SSE/EventSource code.
      try {
        const es = new EventSource(`${this._url}/bridge/events`);
        es.onmessage = (e) => {
          let msg;
          try { msg = JSON.parse(e.data); } catch { return; }
          if (msg && typeof msg.event === "string") this._dispatch(msg.event, msg.payload);
        };
        es.onerror = () => { /* EventSource auto-reconnects */ };
        this._es = es;
      } catch (e) {
        console.error("hull bridge: SSE connect failed", e);
      }
    }
  }

  _detectMode() {
    if (typeof window !== "undefined" && typeof window.ping === "function") return "native";
    if (DEV && this._url) return "http";
    return "none";
  }

  invoke(name, ...args) {
    if (this.mode === "native") {
      const fn = window[name];
      if (typeof fn !== "function") {
        return Promise.reject(new Error(`Native binding "${name}" unavailable`));
      }
      return fn(...args);
    }
    if (DEV && this.mode === "http") {
      return fetch(`${this._url}/bridge/invoke`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, args }),
      }).then((r) => r.json());
    }
    return Promise.reject(new Error(`Native binding "${name}" unavailable (no host)`));
  }

  on(event, handler) {
    if (!this._handlers.has(event)) this._handlers.set(event, new Set());
    this._handlers.get(event).add(handler);
    return () => this.off(event, handler);
  }

  off(event, handler) {
    this._handlers.get(event)?.delete(handler);
  }

  _dispatch(event, payload) {
    this._handlers.get(event)?.forEach((h) => {
      try { h(payload); } catch (e) { console.error(`bridge handler "${event}"`, e); }
    });
  }
}

export const bridge = new NativeBridge(); // singleton
