import { performance } from "node:perf_hooks";

// Per-command timing. Total is always printed; per-step lines only with -v/--verbose,
// so you can feel where the time goes (e.g. the Vite build vs. archiving).
export function createTimer(verbose) {
  const start = performance.now();
  let prev = start;
  const fmt = (ms) => (ms >= 1000 ? `${(ms / 1000).toFixed(2)}s` : `${Math.round(ms)}ms`);
  return {
    verbose,
    step(label) {
      const now = performance.now();
      if (verbose) {
        console.log(`  · ${label.padEnd(30)} ${fmt(now - prev).padStart(8)}   (elapsed ${fmt(now - start)})`);
      }
      prev = now;
    },
    total(label) {
      console.log(`${label} — ${fmt(performance.now() - start)} total`);
    },
  };
}
