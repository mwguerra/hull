// Type definitions for @mwguerra/hull/vue
import type { Ref } from "vue";

/** A ref two-way-bound to a C++-persisted setting (debounced write-down). */
export function useNativeState<T = unknown>(
  key: string,
  options?: { debounce?: number },
): Ref<T | undefined>;

export { bridge, nativeSetting } from "./bridge";
