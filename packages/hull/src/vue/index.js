// Vue adapter:  import { useNativeState } from "@mwguerra/hull/vue";
import { ref, watch, onScopeDispose } from "vue";
import { nativeSetting } from "../bridge/native-store.js";

// A ref that two-way-binds to a C++-persisted setting.
// Edits flow down to C++ (debounced); C++ pushes flow up into the ref.
export function useNativeState(key, { debounce = 150 } = {}) {
  const store = nativeSetting(key);
  const state = ref(store.get());
  let applying = false; // true while applying a C++-originated value (prevents echo write)
  let timer = null;

  const unsubscribe = store.subscribe((v) => {        // C++ -> UI
    if (v === state.value) return;
    applying = true;
    state.value = v;
    queueMicrotask(() => { applying = false; });
  });

  watch(state, (v) => {                               // UI -> C++ (debounced)
    if (applying) return;
    clearTimeout(timer);
    timer = setTimeout(() => store.set(v).catch(console.error), debounce);
  });

  store.load().catch(() => {});                       // initial pull
  onScopeDispose(() => { unsubscribe(); clearTimeout(timer); });
  return state;
}

export { bridge, nativeSetting } from "../bridge/index.js";
