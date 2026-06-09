import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Normal Vite + React config. `hull build` injects the single-file plugin.
export default defineConfig({
  plugins: [react()],
});
