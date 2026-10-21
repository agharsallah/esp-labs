// The shapes that travel between the board, the Node server and the React app.
// The server validates everything with these schemas; the React app only imports the types.
import { z } from "zod";

// ---- What the board sends (GET /api/status, /api/networks, and its WebSocket) ----------

export const WifiState = z.enum(["connected", "connecting", "failed", "not-set-up"]);

export const Status = z.object({
  firmware: z.string(),
  uptimeS: z.number(),
  heapFree: z.number(),
  psramFree: z.number(),
  tempC: z.number().optional(),
  clients: z.number(),
  wifi: z.object({
    state: WifiState,
    ssid: z.string(),
    hostname: z.string(),
    ip: z.string().optional(),
    rssi: z.number().optional(),
    channel: z.number().optional(),
    reason: z.string(),
  }),
  setup: z.object({ active: z.boolean(), ssid: z.string() }),
  // Only in wifi-lab-w2 builds that know about the remote link (always "off" in variation 1).
  remote: z.object({ state: z.string() }).optional(),
  screen: z.object({
    view: z.string(),
    views: z.array(z.string()),
    autoScan: z.boolean(),
    backlight: z.number(),
    message: z.string(),
  }),
  led: z.object({ r: z.number(), g: z.number(), b: z.number() }),
  scan: z.object({ running: z.boolean(), count: z.number(), durationMs: z.number() }),
});
export type Status = z.infer<typeof Status>;

export const Networks = z.object({
  scans: z.number(),
  durationMs: z.number(),
  networks: z.array(
    z.object({ ssid: z.string(), rssi: z.number(), channel: z.number(), auth: z.string() }),
  ),
});
export type Networks = z.infer<typeof Networks>;

export const ButtonEvent = z.object({
  source: z.string(),
  gesture: z.string(),
  uptimeS: z.number(),
});
export type ButtonEvent = z.infer<typeof ButtonEvent>;

export const EspMessage = z.discriminatedUnion("type", [
  z.object({ type: z.literal("status"), data: Status }),
  z.object({ type: z.literal("networks"), data: Networks }),
  z.object({ type: z.literal("event"), data: ButtonEvent }),
  z.object({ type: z.literal("ota"), data: z.object({ percent: z.number() }) }),
]);
export type EspMessage = z.infer<typeof EspMessage>;

// Remote link only: the board's answer to a request the server sent it (see server/src/relay.ts).
export const RelayReply = z.object({
  type: z.literal("reply"),
  id: z.number().int(),
  status: z.number().int(),
  body: z.unknown(),
});

// ---- What the React app may ask for (validated by the server before it reaches the board) --

const byte = z.number().int().min(0).max(255);

export const ViewBody = z.object({ view: z.string().min(1).max(16) });
export const SettingsBody = z
  .object({ autoScan: z.boolean().optional(), backlight: byte.optional() })
  .refine(
    (b) => b.autoScan !== undefined || b.backlight !== undefined,
    "send autoScan and/or backlight",
  );
export const LedBody = z.object({ r: byte, g: byte, b: byte });
export const MessageBody = z.object({ text: z.string().max(96) });
export const SessionBody = z.object({ key: z.string().min(1).max(200) });
export const WifiBody = z.object({
  ssid: z.string().min(1).max(32),
  password: z
    .string()
    .max(63)
    .refine(
      (p) => p.length === 0 || p.length >= 8,
      "8-63 characters, or empty for an open network",
    ),
});

// ---- What the Node server sends to the React app over its own WebSocket -----------------

export interface HistoryPoint {
  t: number; // ms since epoch, stamped by the server
  rssi?: number;
  tempC?: number;
  heapFree: number;
}

// How the server reaches the board: it dials the board on your Wi-Fi ("lan"), or the board dials
// in to the server's /device, from anywhere ("relay").
export type LinkMode = "lan" | "relay";

export interface Snapshot {
  espConnected: boolean;
  adminEnabled: boolean; // false when ESP_TOKEN is not set
  mode: LinkMode;
  boardAt: string; // where the board is expected: its URL (lan) or the server's /device (relay)
  status: Status | null;
  networks: Networks | null;
  history: HistoryPoint[];
}

export type ServerMessage =
  | { type: "hello"; data: Snapshot }
  | { type: "link"; data: { espConnected: boolean } }
  | { type: "status"; data: Status; point: HistoryPoint }
  | { type: "networks"; data: Networks }
  | { type: "event"; data: ButtonEvent }
  | { type: "ota"; data: { percent: number } };
