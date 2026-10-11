import { EventEmitter } from "node:events";
import type { FastifyBaseLogger } from "fastify";
import { describe, expect, it } from "vitest";
import { type BoardSocket, RelayLink } from "../src/relay.ts";

// A stand-in for the board's WebSocket: records what the server sends, lets the test answer.
class FakeBoard extends EventEmitter {
  sent: string[] = [];
  send(text: string) {
    this.sent.push(text);
  }
  close() {
    this.emit("close");
  }
  reply(json: unknown) {
    this.emit("message", Buffer.from(JSON.stringify(json)));
  }
}

const quiet = { info() {}, warn() {}, debug() {} } as unknown as FastifyBaseLogger;
const noEvents = { onMessage() {}, onLink() {} };

describe("RelayLink", () => {
  it("sends a request down the board's socket and resolves with the matching reply", async () => {
    const link = new RelayLink(quiet, noEvents);
    const board = new FakeBoard();
    link.attach(board as unknown as BoardSocket);

    const answer = link.call("led", { r: 1, g: 2, b: 3 });
    const request = JSON.parse(board.sent[0] ?? "{}");
    expect(request).toMatchObject({ type: "request", method: "PUT", path: "/api/led" });
    board.reply({ type: "reply", id: request.id, status: 200, body: { ok: true } });

    await expect(answer).resolves.toEqual({ status: 200, body: { ok: true } });
  });

  it("fails waiting requests when the board disconnects", async () => {
    const link = new RelayLink(quiet, noEvents);
    const board = new FakeBoard();
    link.attach(board as unknown as BoardSocket);
    const answer = link.call("scan");
    board.close();
    await expect(answer).rejects.toThrow("disconnected");
    expect(link.connected).toBe(false);
  });
});
