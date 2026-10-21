// The dashboard server: REST endpoints for the React app, a WebSocket that relays the board's
// live stream, and (in production) the built React app itself.
//
//   GET    /healthz          {ok, esp: "connected" | "down"}
//   GET    /api/state        everything the app needs to draw itself (Snapshot)
//   POST   /api/scan         -> board POST /api/scan
//   PUT    /api/view         {view}                -> board
//   PUT    /api/settings     {autoScan?, backlight?} -> board
//   PUT    /api/led          {r, g, b}             -> board
//   PUT    /api/message      {text}                -> board
//   PUT    /api/wifi         {ssid, password}      -> board, admin
//   DELETE /api/wifi                               -> board, admin
//   POST   /api/reboot                             -> board, admin
//   POST   /api/ota          raw firmware.bin (application/octet-stream) -> board multipart, admin
//   WS     /ws               ServerMessage stream (see shared/protocol.ts)
//   GET|POST|DELETE /api/session   sign-in state / sign in {key} / sign out (when DASHBOARD_KEY is set)
//   WS     /device           relay mode only: the board dials in here (Bearer ESP_TOKEN)
//
// Every body is validated here before it reaches the board, and admin calls get the token from
// ESP_TOKEN added on the way. The token never reaches the browser. How the board is reached
// (LAN or relay) is hidden behind BoardLink (board.ts).
import { fileURLToPath } from "node:url";
import fastifyStatic from "@fastify/static";
import fastifyWebsocket from "@fastify/websocket";
import Fastify, { type FastifyBaseLogger, type FastifyInstance, type FastifyReply } from "fastify";
import type { z } from "zod";
import { LedBody, MessageBody, SettingsBody, ViewBody, WifiBody } from "../../shared/protocol.ts";
import { bearerMatches, registerAuth } from "./auth.ts";
import type { BoardLink } from "./board.ts";
import type { Config } from "./config.ts";
import type { RouteName } from "./esp.ts";
import type { Hub } from "./hub.ts";
import { RelayLink } from "./relay.ts";

// makeBoard builds the BoardLink (LAN or relay) with the app's logger.
export async function buildApp(
  config: Config,
  hub: Hub,
  makeBoard: (log: FastifyBaseLogger) => BoardLink,
): Promise<{ app: FastifyInstance; board: BoardLink }> {
  const app = Fastify({
    logger: { level: config.logLevel },
    // The tunnel or hosting proxy in front of us says whether the visitor used HTTPS (TRUST_PROXY).
    trustProxy: config.trustProxy,
  });
  const board = makeBoard(app.log);
  hub.describeBoard(board);
  await app.register(fastifyWebsocket);
  registerAuth(app, config.dashboardKey);

  // Firmware uploads arrive as raw bytes. The board's app partition is 6.25 MB; 8 MB is plenty.
  app.addContentTypeParser(
    "application/octet-stream",
    { parseAs: "buffer", bodyLimit: 8 * 1024 * 1024 },
    (_req, body, done) => done(null, body),
  );

  // Forward one validated request to the board and pass its answer back.
  async function forward(
    reply: FastifyReply,
    route: RouteName,
    body?: unknown,
  ): Promise<FastifyReply> {
    try {
      const res = await board.call(route, body);
      return reply.code(res.status).send(res.body);
    } catch (err) {
      app.log.warn({ route, err: (err as Error).message }, "board did not answer");
      return reply.code(502).send({
        error:
          board.mode === "lan"
            ? `The board did not answer at ${board.describe}. Is it on and joined to your network?`
            : "The board is not connected to this server right now. Is wifi-lab-w2-remote running, with this server's tunnel address set (serial: relay wss://…)?",
      });
    }
  }

  function validated<T extends z.ZodType>(
    schema: T,
    body: unknown,
    reply: FastifyReply,
  ): z.infer<T> | undefined {
    const parsed = schema.safeParse(body);
    if (parsed.success) return parsed.data;
    void reply.code(400).send({ error: parsed.error.issues.map((i) => i.message).join("; ") });
    return undefined;
  }

  function adminReady(reply: FastifyReply): boolean {
    if (config.espToken) return true;
    void reply.code(503).send({
      error:
        "Admin actions are off: set ESP_TOKEN in esp-labs/apps/dashboard/.env (board serial command: token)",
    });
    return false;
  }

  app.get("/healthz", async () => ({
    ok: true,
    esp: hub.espConnected ? "connected" : "down",
    clients: hub.clientCount,
  }));
  app.get("/api/state", async () => hub.snapshot());

  app.post("/api/scan", (_req, reply) => forward(reply, "scan"));
  app.put("/api/view", (req, reply) => {
    const body = validated(ViewBody, req.body, reply);
    return body ? forward(reply, "view", body) : reply;
  });
  app.put("/api/settings", (req, reply) => {
    const body = validated(SettingsBody, req.body, reply);
    return body ? forward(reply, "settings", body) : reply;
  });
  app.put("/api/led", (req, reply) => {
    const body = validated(LedBody, req.body, reply);
    return body ? forward(reply, "led", body) : reply;
  });
  app.put("/api/message", (req, reply) => {
    const body = validated(MessageBody, req.body, reply);
    return body ? forward(reply, "message", body) : reply;
  });
  app.put("/api/wifi", (req, reply) => {
    if (!adminReady(reply)) return reply;
    const body = validated(WifiBody, req.body, reply);
    return body ? forward(reply, "saveWifi", body) : reply;
  });
  app.delete("/api/wifi", (_req, reply) =>
    adminReady(reply) ? forward(reply, "forgetWifi") : reply,
  );
  app.post("/api/reboot", (_req, reply) => (adminReady(reply) ? forward(reply, "reboot") : reply));

  app.post("/api/ota", async (req, reply) => {
    if (!adminReady(reply)) return reply;
    const firmware = req.body;
    if (!Buffer.isBuffer(firmware) || firmware.length < 1024)
      return reply
        .code(400)
        .send({ error: "send the firmware.bin file as application/octet-stream" });
    // ESP32 app images start with the magic byte 0xE9; refuse anything else before it reaches the board.
    if (firmware[0] !== 0xe9)
      return reply.code(400).send({
        error: "that file is not an ESP32 firmware image (.pio/build/wifi-lab-w2/firmware.bin)",
      });
    try {
      const res = await board.uploadFirmware(firmware);
      app.log.info(
        { bytes: firmware.length, status: res.status },
        "firmware forwarded to the board",
      );
      return reply.code(res.status).send(res.body);
    } catch (err) {
      return reply
        .code(502)
        .send({ error: `Upload to the board failed: ${(err as Error).message}` });
    }
  });

  app.get("/ws", { websocket: true }, (socket) => hub.addClient(socket));

  if (board instanceof RelayLink) {
    // The board's way in. The token is checked before the WebSocket upgrade, so a wrong one
    // never gets a socket.
    app.get(
      "/device",
      {
        websocket: true,
        preValidation: async (req, reply) => {
          if (bearerMatches(req.headers.authorization, config.espToken)) return;
          app.log.warn({ ip: req.ip }, "refused a /device connection: wrong or missing token");
          return reply.code(401).send({ error: "wrong token" });
        },
      },
      (socket) => board.attach(socket),
    );
  }

  if (config.production) {
    await app.register(fastifyStatic, {
      root: fileURLToPath(new URL("../../dist/web", import.meta.url)),
    });
  }
  return { app, board };
}
