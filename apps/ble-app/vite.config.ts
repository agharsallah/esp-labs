import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// A static app: Vite only serves the files. The browser talks to the board itself, over Bluetooth.
// Web Bluetooth needs a secure page: http://localhost on the Mac counts as secure; a phone needs
// https, which `pnpm tunnel` gives (a cloudflared quick tunnel to this port).
export default defineConfig({
  plugins: [react()],
  server: { port: 5177, host: "localhost", allowedHosts: [".trycloudflare.com"] },
  preview: { port: 5177, host: "localhost", allowedHosts: [".trycloudflare.com"] },
});
