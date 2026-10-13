// Live state from the board: one WebSocket (status every second, the card list when it changes,
// each access event, job results), plus a first fetch so the page isn't empty while it connects.
// Jobs (enroll, label, wipe, inspect, open) answer 202 {job: id}; runJob() waits for the "job"
// message with that id.
import { useCallback, useEffect, useRef, useState } from "react";
import { api, type Card, type DoorEvent, type JobResult, Message, type Status } from "./api.ts";

const JOB_TIMEOUT_MS = 30_000;

export function useDoor() {
  const [status, setStatus] = useState<Status | null>(null);
  const [cards, setCards] = useState<Card[]>([]);
  const [events, setEvents] = useState<DoorEvent[]>([]);
  const [connected, setConnected] = useState(false);
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
        else if (m.type === "cards") setCards(m.data);
        else if (m.type === "event")
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
      .catch(() => {});
    return () => {
      stopped = true;
      clearTimeout(retry);
      socket?.close();
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

  return { status, cards, events, connected, runJob };
}
