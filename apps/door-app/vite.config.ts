import react from "@vitejs/plugin-react";
import { defineConfig, loadEnv } from "vite";

// The app talks to the board through Vite's dev server: same origin for the browser (no CORS),
// and the admin token is added here, on the Mac, from .env. The browser never sees it.
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "BENCH_");
  const target = env.BENCH_URL || "http://bench.local";
  const auth = env.BENCH_TOKEN ? { Authorization: `Bearer ${env.BENCH_TOKEN}` } : undefined;
  return {
    plugins: [react()],
    server: {
      port: 5174,
      proxy: {
        "/api": { target, changeOrigin: true, headers: auth },
        "/ws": { target: target.replace(/^http/, "ws"), ws: true, changeOrigin: true },
      },
    },
  };
});
