// One WebSocket to the dashboard server, turned into React state. Reconnects by itself.
import { useEffect, useReducer } from "react";
import type { HistoryPoint, ServerMessage, Snapshot } from "../../shared/protocol.ts";

export interface FeedItem {
  id: number;
  time: Date;
  kind: "button" | "link" | "scan" | "update";
  text: string;
}

export interface BenchState {
  serverUp: boolean; // our own server, not the board
  snapshot: Snapshot | null;
  feed: FeedItem[];
  otaPercent: number | null;
}

type Action = { type: "server"; up: boolean } | { type: "message"; message: ServerMessage };

const HISTORY = 600;
const FEED = 40;
let nextId = 1;

function note(state: BenchState, kind: FeedItem["kind"], text: string): FeedItem[] {
  return [{ id: nextId++, time: new Date(), kind, text }, ...state.feed].slice(0, FEED);
}

function reduce(state: BenchState, action: Action): BenchState {
  if (action.type === "server") return { ...state, serverUp: action.up };
  const m = action.message;
  const snap = state.snapshot;
  switch (m.type) {
    case "hello":
      return { ...state, snapshot: m.data };
    case "link":
      if (!snap) return state;
      return {
        ...state,
        snapshot: { ...snap, espConnected: m.data.espConnected },
        feed: note(state, "link", m.data.espConnected ? "Board connected" : "Board disconnected"),
        otaPercent: m.data.espConnected ? null : state.otaPercent,
      };
    case "status": {
      if (!snap) return state;
      const history: HistoryPoint[] = [...snap.history, m.point].slice(-HISTORY);
      return { ...state, snapshot: { ...snap, status: m.data, history } };
    }
    case "networks":
      if (!snap) return state;
      return {
        ...state,
        snapshot: { ...snap, networks: m.data },
        feed: note(
          state,
          "scan",
          `Scan finished: ${m.data.networks.length} networks in ${m.data.durationMs} ms`,
        ),
      };
    case "event":
      return { ...state, feed: note(state, "button", `${m.data.source}: ${m.data.gesture}`) };
    case "ota":
      return {
        ...state,
        otaPercent: m.data.percent,
        feed:
          m.data.percent === 100
            ? note(state, "update", "Firmware written, the board is rebooting")
            : state.feed,
      };
  }
}

export function useBench(): BenchState {
  const [state, dispatch] = useReducer(reduce, {
    serverUp: false,
    snapshot: null,
    feed: [],
    otaPercent: null,
  });

  useEffect(() => {
    let socket: WebSocket | undefined;
    let retry: number | undefined;
    let closed = false;
    const open = () => {
      socket = new WebSocket(
        `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`,
      );
      socket.onopen = () => dispatch({ type: "server", up: true });
      socket.onmessage = (e) =>
        dispatch({ type: "message", message: JSON.parse(String(e.data)) as ServerMessage });
      socket.onclose = () => {
        dispatch({ type: "server", up: false });
        if (!closed) retry = window.setTimeout(open, 2000);
      };
    };
    open();
    return () => {
      closed = true;
      window.clearTimeout(retry);
      socket?.close();
    };
  }, []);

  return state;
}
