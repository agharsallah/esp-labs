// Live state from the board: one WebSocket (status every second, the card list and the saved
// card data when they change, each access event, job results), plus a first fetch so the page
// isn't empty while it connects. Jobs (enroll, replace, label, wipe, inspect, save, restore, copy,
// open) answer 202 {job: id}; runJob() waits for the "job" message with that id.
import { useCallback, useEffect, useRef, useState } from "react";
import {
  api,
  type Card,
  type DoorEvent,
  type JobResult,
  Message,
  type Snapshot,
  type Status,
} from "./api.ts";

const JOB_TIMEOUT_MS = 30_000;

export function useDoor() {
  const [status, setStatus] = useState<Status | null>(null);
  const [cards, setCards] = useState<Card[]>([]);
  const [snapshots, setSnapshots] = useState<Snapshot[]>([]);
  const [events, setEvents] = useState<DoorEvent[]>([]);
  const [connected, setConnected] = useState(false);
  // What has arrived at least once, so the panels show placeholders until then, not "nothing yet".
  const [loaded, setLoaded] = useState({ cards: false, snapshots: false, events: false });
  const arrived = (part: keyof typeof loaded) =>
    setLoaded((l) => (l[part] ? l : { ...l, [part]: true }));
  const waiting = useRef(new Map<number, (r: JobResult) => void>());
  const early = useRef(new Map<number, JobResult>()); // results that beat their POST's answer

  useEffect(() => {
    let socket: WebSocket | null = null;
    let retry: ReturnType<typeof setTimeout> | undefined;
    let stopped = false;
    const connect = () => {
      socket = new WebSocket(
        `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`,
      );
      socket.onopen = () => setConnected(true);
      socket.onclose = () => {
        setConnected(false);
        if (!stopped) retry = setTimeout(connect, 2000);
      };
      socket.onmessage = (e) => {
        const parsed = Message.safeParse(JSON.parse(String(e.data)));
        if (!parsed.success)
          return console.warn("message the app doesn't understand", parsed.error.issues);
        const m = parsed.data;
        if (m.type === "status") setStatus(m.data);
        else if (m.type === "cards") {
          setCards(m.data);
          arrived("cards");
        } else if (m.type === "snapshots") {
          setSnapshots(m.data);
          arrived("snapshots");
        } else if (m.type === "event")
          setEvents((list) =>
            list.some((x) => x.seq === m.data.seq) ? list : [...list, m.data].slice(-100),
          );
        else {
          const done = waiting.current.get(m.data.id);
          if (done) {
            waiting.current.delete(m.data.id);
            done(m.data);
          } else early.current.set(m.data.id, m.data);
        }
      };
    };
    connect();
    api
      .events()
      .then(setEvents)
      .catch(() => {})
      .finally(() => arrived("events"));
    return () => {
      stopped = true;
      clearTimeout(retry);
      // Closing a socket that is still connecting makes Vite's proxy log "ws proxy error: EPIPE"
      // (React's StrictMode mounts twice in development): let it open, then close it.
      const s = socket;
      if (!s) return;
      s.onclose = null; // this socket's end must not mark the next one as disconnected
      s.onmessage = null;
      if (s.readyState === WebSocket.CONNECTING) s.onopen = () => s.close();
      else s.close();
    };
  }, []);

  // Start a job and wait for its result (or 30 s).
  const runJob = useCallback(async (start: () => Promise<number>): Promise<JobResult> => {
    const id = await start();
    const already = early.current.get(id);
    if (already) {
      early.current.delete(id);
      return already;
    }
    return new Promise((resolve) => {
      waiting.current.set(id, resolve);
      setTimeout(() => {
        if (!waiting.current.delete(id)) return;
        resolve({ id, ok: false, message: "no answer from the board within 30 s" });
      }, JOB_TIMEOUT_MS);
    });
  }, []);

  return { status, cards, snapshots, events, connected, loaded, runJob };
}
