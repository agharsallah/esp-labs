// The real board: one WebSocket for everything it pushes (status every second, the card list
// when it changes, every show), plain fetch for changes. Both go through Vite's proxy, which adds
// the admin token, so the browser never holds it. Reconnects every 2 s while the board is away.
import { z } from "zod";
import { type BoardLink, BoardError, type CardPatch, FriendCard, Message } from "./types.ts";

async function call(method: string, path: string, body?: unknown): Promise<unknown> {
  let res: Response;
  try {
    res = await fetch(path, {
      method,
      headers: body === undefined ? undefined : { "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new BoardError(0, "The board isn't reachable. Is rfid-lab-r8 on your Wi-Fi?");
  }
  const text = await res.text();
  let json: unknown = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch {
    throw new BoardError(
      res.status,
      `The board answered something that isn't JSON (HTTP ${res.status}).`,
    );
  }
  if (!res.ok) {
    if (res.status === 401)
      throw new BoardError(
        401,
        "The admin token is missing or wrong: set BENCH_TOKEN in friends-app/.env (serial: token), then restart pnpm dev.",
      );
    const message =
      typeof json === "object" && json !== null && "error" in json
        ? String(json.error)
        : `HTTP ${res.status}`;
    throw new BoardError(res.status, message);
  }
  return json;
}

const q = (uid: string) => `?uid=${encodeURIComponent(uid)}`;

export function liveLink(): BoardLink {
  return {
    kind: "live",
    subscribe(onMessage, onConnected) {
      let socket: WebSocket | null = null;
      let retry: ReturnType<typeof setTimeout> | undefined;
      let stopped = false;
      const connect = () => {
        socket = new WebSocket(
          `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`,
        );
        socket.onopen = () => onConnected(true);
        socket.onclose = () => {
          onConnected(false);
          if (!stopped) retry = setTimeout(connect, 2000);
        };
        socket.onmessage = (e) => {
          let raw: unknown;
          try {
            raw = JSON.parse(String(e.data));
          } catch {
            return console.warn("[board] a message that isn't JSON", e.data);
          }
          const parsed = Message.safeParse(raw);
          if (!parsed.success)
            return console.warn(
              "[board] a message the app doesn't understand",
              parsed.error.issues,
            );
          onMessage(parsed.data);
        };
      };
      connect();
      return () => {
        stopped = true;
        clearTimeout(retry);
        // Closing a socket mid-handshake makes Vite's proxy log EPIPE (StrictMode mounts twice in
        // development): let it open, then close it. Same as the door-app.
        const s = socket;
        if (!s) return;
        s.onclose = null;
        s.onmessage = null;
        if (s.readyState === WebSocket.CONNECTING) s.onopen = () => s.close();
        else s.close();
      };
    },
    update: async (uid: string, patch: CardPatch) =>
      z.object({ card: FriendCard }).parse(await call("PUT", `/api/card${q(uid)}`, patch)).card,
    forget: async (uid: string) => void (await call("DELETE", `/api/card${q(uid)}`)),
    show: async (uid: string) => void (await call("POST", `/api/show${q(uid)}`)),
    chipReset: async () => void (await call("POST", "/api/chip-reset")),
  };
}
