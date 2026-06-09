import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

// The inspector ships as ONE self-contained HTML (dist/inspector.html) that the dev
// server serves at /__hull/devtools. Built by `npm run build:devtools`.
export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  build: {
    outDir: "dist",
    cssCodeSplit: false,
    target: "esnext",
    emptyOutDir: true,
  },
});
