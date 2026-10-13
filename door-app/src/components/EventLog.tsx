import type { DoorEvent } from "../api.ts";
import { tone } from "../rules.ts";

// The access log, newest first: every tap with its decision, and every change made from the app.
export function EventLog({ events }: { events: DoorEvent[] }) {
  if (!events.length) return <p className="empty">Nothing yet. Tap a card on the reader.</p>;
  return (
    <ol className="log" aria-live="polite">
      {[...events].reverse().map((e) => {
        const tap = e.what === "tap";
        return (
          <li key={e.seq}>
            <time className="mono">
              {e.at ? new Date(e.at * 1000).toLocaleTimeString() : "no clock"}
            </time>
            <span className="log-who">
              {e.name || <span className="mono">{e.uid || "–"}</span>}
            </span>
            <span className={`now ${tap ? tone(e.decision) : "info"}`}>
              {tap ? e.decision : e.what}
            </span>
          </li>
        );
      })}
    </ol>
  );
}
