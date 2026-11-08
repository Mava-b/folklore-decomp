import { defineConfig } from "vite";

// Tauri expects a fixed port and no screen clearing (its own logs share the terminal).
export default defineConfig({
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: { ignored: ["**/src-tauri/**"] },
  },
  build: {
    target: "safari15",
    outDir: "dist",
  },
});
