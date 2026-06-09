// React adapter:  import { useNativeState } from "@mwguerra/hull/react";
import { useEffect, useRef, useState, useCallback } from "react";
import { nativeSetting } from "../bridge/native-store.js";

// Like useState, but two-way-bound to a C++-persisted setting.
// Returns [value, setValue]; setValue writes through to C++ (debounced).
export function useNativeState(key, { debounce = 150 } = {}) {
  const storeRef = useRef(null);
  if (!storeRef.current) storeRef.current = nativeSetting(key);
  const store = storeRef.current;

  const [value, setLocal] = useState(store.get());
  const timer = useRef(null);

  useEffect(() => {                          // C++ -> UI
    const unsub = store.subscribe(setLocal);
    store.load().catch(() => {});
    return unsub;
  }, [store]);

  const setValue = useCallback((v) => {      // UI -> C++ (debounced)
    setLocal(v);
    clearTimeout(timer.current);
    timer.current = setTimeout(() => store.set(v).catch(console.error), debounce);
  }, [store, debounce]);

  return [value, setValue];
}

export { bridge, nativeSetting } from "../bridge/index.js";
