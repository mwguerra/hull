// Type definitions for @mwguerra/hull/react

/** Like useState, but two-way-bound to a C++-persisted setting. */
export function useNativeState<T = unknown>(
  key: string,
  options?: { debounce?: number },
): [T | undefined, (value: T) => void];

export { bridge, nativeSetting } from "./bridge";
