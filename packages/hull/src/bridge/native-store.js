import { bridge } from "./bridge-core.js";

// A framework-agnostic, two-way store for one setting key.
//  - load():       pull the current value from C++
//  - get():        cached value
//  - set(v):       write through to C++ (persisted + encrypted) and update locally
//  - subscribe(cb): observe changes from EITHER direction; returns an unsubscribe fn
// C++ emits "settings:changed" {key, value} after any successful write, keeping every
// subscriber in sync (including other components/windows).
export function nativeSetting(key) {
  let value;
  const subs = new Set();
  const notify = () => subs.forEach((cb) => cb(value));

  bridge.on("settings:changed", (p) => {
    if (p && p.key === key) { value = p.value; notify(); } // C++ -> store
  });

  return {
    get: () => value,
    async load() {
      const res = await bridge.invoke("loadSetting", key);
      if (res?.ok) { value = res.value; notify(); }
      return value;
    },
    async set(v) {
      value = v; notify();                                   // optimistic local update
      const res = await bridge.invoke("saveSetting", key, v);
      if (!res?.ok) throw new Error(res?.error ?? "saveSetting failed");
      return value;                                          // C++ echo re-syncs others
    },
    subscribe(cb) { subs.add(cb); return () => subs.delete(cb); },
  };
}
