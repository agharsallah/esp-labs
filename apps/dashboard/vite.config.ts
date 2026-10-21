import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

const server = `http://localhost:${process.env.PORT ?? "8787"}`;

// The React app lives in web/. In development Vite serves it and forwards the API and the
// live WebSocket to the Node server; in production the Node server serves dist/web itself.
export default defineConfig({
  root: "web",
  plugins: [react()],
  build: { outDir: "../dist/web", emptyOutDir: true },
  server: {
    port: 5173,
    proxy: {
      "/api": server,
      "/healthz": server,
      "/ws": { target: server.replace("http", "ws"), ws: true },
    },
  },
});
