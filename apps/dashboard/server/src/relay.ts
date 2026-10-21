// Relay mode: the board dials in to the server's /device and keeps one WebSocket open, from
// anywhere (through a cloudflared or ngrok tunnel). The board firmware side is
// esp-labs/labs/wifi-lab-w2/src/remote/relay.cpp.
//
// Requests go down that socket with an id; the board answers {type: "reply", id, status, body}.
// Everything else it sends is the same stream as its own /ws (status, networks, event).
// One board at a time: a new connection (after a reboot, say) replaces the old one.
import type { FastifyBaseLogger } from "fastify";
import type { WebSocket } from "ws";
import { EspMessage, RelayReply } from "../../shared/protocol.ts";
import type { BoardEvents, BoardLink, BoardReply } from "./board.ts";
import { ROUTES, type RouteName } from "./esp.ts";

interface Pending {
  resolve: (reply: BoardReply) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

// The socket as far as the relay is concerned, so tests can pass a stand-in.
export type BoardSocket = Pick<WebSocket, "send" | "close" | "on">;

export class RelayLink implements BoardLink {
  readonly mode = "relay";
  readonly describe = "/device (the board dials in)";
  private socket: BoardSocket | undefined;
  private nextId = 1;
  private readonly pending = new Map<number, Pending>();
  private readonly log: FastifyBaseLogger;
  private readonly events: BoardEvents;
  private readonly timeoutMs: number;

  constructor(log: FastifyBaseLogger, events: BoardEvents, timeoutMs = 8000) {
    this.log = log;
    this.events = events;
    this.timeoutMs = timeoutMs;
  }

  get connected(): boolean {
    return this.socket !== undefined;
  }

  // Called by the /device route once the board's token checked out.
  attach(socket: BoardSocket): void {
    if (this.socket) {
      this.log.info("a new board connection replaces the old one");
      this.socket.close(4000, "replaced by a newer connection");
    }
    this.socket = socket;
    this.log.info("board connected to /device");
    this.events.onLink(true);
    socket.on("message", (raw: Buffer) => this.receive(raw.toString()));
    socket.on("error", (err: Error) => this.log.debug({ err: err.message }, "board socket error"));
    socket.on("close", () => {
      if (this.socket !== socket) return; // an older, replaced connection
      this.socket = undefined;
      this.failPending("the board disconnected");
      this.log.warn("board disconnected from /device; it retries every 5 s");
      this.events.onLink(false);
    });
  }

  call(route: RouteName, body?: unknown): Promise<BoardReply> {
    const socket = this.socket;
    if (!socket) return Promise.reject(new Error("the board is not connected to /device"));
    const { method, path } = ROUTES[route];
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`the board did not answer within ${this.timeoutMs / 1000} s`));
      }, this.timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      socket.send(JSON.stringify({ type: "request", id, method, path, body: body ?? {} }));
    });
  }

  uploadFirmware(): Promise<BoardReply> {
    return Promise.resolve({
      status: 501,
      body: {
        error:
          "Firmware updates need ESP_MODE=lan for now (the board's own web server receives the file).",
      },
    });
  }

  start(): void {
    // Nothing to open: the board comes to us.
  }

  stop(): void {
    this.socket?.close(1001, "server shutting down");
    this.failPending("the server is shutting down");
  }

  private receive(text: string): void {
    let json: unknown;
    try {
      json = JSON.parse(text);
    } catch {
      this.log.warn("board sent something that is not JSON");
      return;
    }
    const reply = RelayReply.safeParse(json);
    if (reply.success) {
      const p = this.pending.get(reply.data.id);
      if (!p) return; // answered after its timeout
      clearTimeout(p.timer);
      this.pending.delete(reply.data.id);
      p.resolve({ status: reply.data.status, body: reply.data.body });
      return;
    }
    const message = EspMessage.safeParse(json);
    if (message.success) this.events.onMessage(message.data);
    else
      this.log.warn(
        { issues: message.error.issues.slice(0, 3) },
        "board message did not match the protocol",
      );
  }

  private failPending(reason: string): void {
    for (const [id, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new Error(reason));
      this.pending.delete(id);
    }
  }
}
