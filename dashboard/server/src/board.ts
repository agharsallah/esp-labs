// The one thing app.ts needs from "the board": send it a request, know whether it's there.
// Two ways to reach it, chosen by ESP_MODE:
//   lan    EspLink (esp.ts): the server dials the board at ESP_URL on your Wi-Fi
//   relay  RelayLink (relay.ts): the board dials in to the server's /device, from anywhere
import type { EspMessage } from "../../shared/protocol.ts";
import type { RouteName } from "./esp.ts";

export interface BoardReply {
  status: number;
  body: unknown;
}

export interface BoardEvents {
  onMessage: (message: EspMessage) => void;
  onLink: (connected: boolean) => void;
}

export interface BoardLink {
  readonly mode: "lan" | "relay";
  readonly connected: boolean;
  // Where the board is expected, for messages: its URL, or "the board dials in to /device".
  readonly describe: string;
  call(route: RouteName, body?: unknown): Promise<BoardReply>;
  // Firmware updates: only over the LAN for now (8 MB through a tunnel is a later lesson).
  uploadFirmware(firmware: Buffer): Promise<BoardReply>;
  start(): void;
  stop(): void;
}
