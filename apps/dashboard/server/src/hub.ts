// The hub remembers the latest picture of the board (status, networks, 10 minutes of history)
// and fans every update out to the browsers that are open on the dashboard.
import type { WebSocket } from "ws";
import type {
  EspMessage,
  HistoryPoint,
  LinkMode,
  ServerMessage,
  Snapshot,
  Status,
  Networks,
} from "../../shared/protocol.ts";

export class Hub {
  private status: Status | null = null;
  private networks: Networks | null = null;
  private readonly history: HistoryPoint[] = [];
  private readonly clients = new Set<WebSocket>();
  espConnected = false;
  private readonly historySize: number;
  private readonly adminEnabled: boolean;
  private board: { mode: LinkMode; describe: string } = { mode: "lan", describe: "" };

  constructor(historySize: number, adminEnabled: boolean) {
    this.historySize = historySize;
    this.adminEnabled = adminEnabled;
  }

  // How the board is reached (the BoardLink), shown in the app's link badge.
  describeBoard(board: { mode: LinkMode; describe: string }): void {
    this.board = board;
  }

  snapshot(): Snapshot {
    return {
      espConnected: this.espConnected,
      adminEnabled: this.adminEnabled,
      mode: this.board.mode,
      boardAt: this.board.describe,
      status: this.status,
      networks: this.networks,
      history: [...this.history],
    };
  }

  addClient(socket: WebSocket): void {
    this.clients.add(socket);
    socket.on("close", () => this.clients.delete(socket));
    this.send(socket, { type: "hello", data: this.snapshot() });
  }

  setLink(connected: boolean): void {
    this.espConnected = connected;
    this.broadcast({ type: "link", data: { espConnected: connected } });
  }

  // Turns a board message into dashboard state + a broadcast.
  accept(message: EspMessage, now = Date.now()): void {
    switch (message.type) {
      case "status": {
        this.status = message.data;
        const point: HistoryPoint = {
          t: now,
          rssi: message.data.wifi.rssi,
          tempC: message.data.tempC,
          heapFree: message.data.heapFree,
        };
        this.history.push(point);
        if (this.history.length > this.historySize)
          this.history.splice(0, this.history.length - this.historySize);
        this.broadcast({ type: "status", data: message.data, point });
        break;
      }
      case "networks":
        this.networks = message.data;
        this.broadcast({ type: "networks", data: message.data });
        break;
      case "event":
        this.broadcast({ type: "event", data: message.data });
        break;
      case "ota":
        this.broadcast({ type: "ota", data: message.data });
        break;
    }
  }

  get clientCount(): number {
    return this.clients.size;
  }

  private broadcast(message: ServerMessage): void {
    const text = JSON.stringify(message);
    for (const socket of this.clients) if (socket.readyState === socket.OPEN) socket.send(text);
  }

  private send(socket: WebSocket, message: ServerMessage): void {
    socket.send(JSON.stringify(message));
  }
}
