import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";

// Normal Vite + Vue config. `hull build` injects the single-file plugin.
export default defineConfig({
  plugins: [vue()],
});
