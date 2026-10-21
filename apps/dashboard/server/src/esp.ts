// LAN mode: the server talks to the board on your Wi-Fi. One REST call helper and one
// long-lived WebSocket to the board's /ws.
import WebSocket from "ws";
import type { FastifyBaseLogger } from "fastify";
import { EspMessage } from "../../shared/protocol.ts";
import type { BoardEvents, BoardLink, BoardReply } from "./board.ts";

// The board's write endpoints. `admin` ones need the token (see the firmware's api_token.h).
export const ROUTES = {
  scan: { method: "POST", path: "/api/scan", admin: false },
  view: { method: "PUT", path: "/api/view", admin: false },
  settings: { method: "PUT", path: "/api/settings", admin: false },
  led: { method: "PUT", path: "/api/led", admin: false },
  message: { method: "PUT", path: "/api/message", admin: false },
  saveWifi: { method: "PUT", path: "/api/wifi", admin: true },
  forgetWifi: { method: "DELETE", path: "/api/wifi", admin: true },
  reboot: { method: "POST", path: "/api/reboot", admin: true },
} as const;
export type RouteName = keyof typeof ROUTES;

export interface EspRequest {
  url: string;
  init: RequestInit;
}

// Pure, so it can be unit-tested: the token is only ever attached to admin routes.
export function buildEspRequest(
  espUrl: string,
  route: RouteName,
  body: unknown,
  token: string | undefined,
): EspRequest {
  const r = ROUTES[route];
  const headers: Record<string, string> = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (r.admin && token) headers.Authorization = `Bearer ${token}`;
  return {
    url: `${espUrl}${r.path}`,
    init: {
      method: r.method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    },
  };
}

export async function callEsp(req: EspRequest, timeoutMs = 5000): Promise<BoardReply> {
  const res = await fetch(req.url, { ...req.init, signal: AbortSignal.timeout(timeoutMs) });
  const body: unknown = await res.json().catch(() => ({}));
  return { status: res.status, body };
}

// ---- Live link -----------------------------------------------------------------------------

// Keeps a WebSocket open to the board's /ws and reconnects with backoff (1 s, 2 s, 4 s ... 15 s)
// whenever it drops: the board rebooting, leaving Wi-Fi, or being unplugged.
export class EspLink implements BoardLink {
  readonly mode = "lan";
  private socket: WebSocket | undefined;
  private retryMs = 1000;
  private timer: NodeJS.Timeout | undefined;
  private stopped = false;
  connected = false;
  private readonly espUrl: string;
  private readonly token: string | undefined;
  private readonly log: FastifyBaseLogger;
  private readonly events: BoardEvents;

  constructor(
    espUrl: string,
    token: string | undefined,
    log: FastifyBaseLogger,
    events: BoardEvents,
  ) {
    this.espUrl = espUrl;
    this.token = token;
    this.log = log;
    this.events = events;
  }

  get describe(): string {
    return this.espUrl;
  }

  call(route: RouteName, body?: unknown): Promise<BoardReply> {
    return callEsp(buildEspRequest(this.espUrl, route, body, this.token));
  }

  async uploadFirmware(firmware: Buffer): Promise<BoardReply> {
    const form = new FormData();
    form.append("firmware", new Blob([new Uint8Array(firmware)]), "firmware.bin");
    const res = await fetch(`${this.espUrl}/api/ota`, {
      method: "POST",
      headers: { Authorization: `Bearer ${this.token}` },
      body: form,
      signal: AbortSignal.timeout(120_000),
    });
    return { status: res.status, body: await res.json().catch(() => ({})) };
  }

  start(): void {
    this.stopped = false;
    this.open();
  }

  stop(): void {
    this.stopped = true;
    clearTimeout(this.timer);
    this.socket?.close();
  }

  private open(): void {
    const url = `${this.espUrl.replace(/^http/, "ws")}/ws`;
    const socket = new WebSocket(url, { handshakeTimeout: 5000 });
    this.socket = socket;

    socket.on("open", () => {
      this.retryMs = 1000;
      this.setConnected(true);
      this.log.info({ url }, "connected to the board");
    });
    socket.on("message", (raw) => {
      let json: unknown;
      try {
        json = JSON.parse(raw.toString());
      } catch {
        this.log.warn("board sent something that is not JSON");
        return;
      }
      const parsed = EspMessage.safeParse(json);
      if (parsed.success) this.events.onMessage(parsed.data);
      else
        this.log.warn(
          { issues: parsed.error.issues.slice(0, 3) },
          "board message did not match the protocol",
        );
    });
    socket.on("error", (err) => this.log.debug({ err: err.message }, "board socket error"));
    socket.on("close", () => {
      if (this.connected) this.log.warn("lost the board, reconnecting");
      this.setConnected(false);
      if (this.stopped) return;
      this.timer = setTimeout(() => this.open(), this.retryMs);
      this.retryMs = Math.min(this.retryMs * 2, 15_000);
    });
  }

  private setConnected(value: boolean): void {
    if (this.connected === value) return;
    this.connected = value;
    this.events.onLink(value);
  }
}
