// Starts the dashboard server: load config, pick how to reach the board (ESP_MODE), listen,
// and shut down cleanly on Ctrl+C / SIGTERM.
import { buildApp } from "./app.ts";
import type { BoardEvents } from "./board.ts";
import { loadConfig } from "./config.ts";
import { EspLink } from "./esp.ts";
import { Hub } from "./hub.ts";
import { RelayLink } from "./relay.ts";

const config = loadConfig();
const hub = new Hub(config.historySize, config.espToken !== undefined);
const events: BoardEvents = {
  onMessage: (message) => hub.accept(message),
  onLink: (connected) => hub.setLink(connected),
};
const { app, board } = await buildApp(config, hub, (log) =>
  config.mode === "relay"
    ? new RelayLink(log, events)
    : new EspLink(config.espUrl, config.espToken, log, events),
);

board.start();
await app.listen({ port: config.port, host: config.host });
app.log.info(
  {
    mode: config.mode,
    board: board.describe,
    admin: config.espToken ? "on" : "off (set ESP_TOKEN)",
    signIn: config.dashboardKey ? "on (DASHBOARD_KEY)" : "off",
  },
  config.production
    ? `dashboard at http://${config.host}:${config.port}`
    : "API ready; open the React app at http://localhost:5173",
);
if (config.mode === "relay")
  app.log.info(
    "relay mode: start a tunnel to this port (pnpm tunnel:cloudflare or pnpm tunnel:ngrok), then type on the board: relay wss://<tunnel host>",
  );

let closing = false;
async function shutdown(signal: string): Promise<void> {
  if (closing) return;
  closing = true;
  app.log.info({ signal }, "shutting down");
  board.stop();
  await app.close();
  process.exit(0);
}
process.on("SIGINT", () => void shutdown("SIGINT"));
process.on("SIGTERM", () => void shutdown("SIGTERM"));
