import { useEffect, useState } from "react";

import type { LogEntry } from "../ble/link.ts";
import { useLink } from "../ble/useLink.ts";
import { opTone } from "./ops.ts";

// The air between the app and the board: every operation crosses it as a packet. Writes and
// subscriptions fly towards the board, reads come back and notifications arrive from it.
export function AirStrip() {
  const l = useLink();
  const [flying, setFlying] = useState<LogEntry[]>([]);
  const last = l.log[l.log.length - 1];

  useEffect(() => {
    if (!last || last.dir === "info") return;
    setFlying((f) => [...f.slice(-11), last]);
    const t = setTimeout(() => setFlying((f) => f.filter((x) => x.id !== last.id)), 1100);
    return () => clearTimeout(t);
  }, [last]);

  return (
    <div className="air" aria-hidden="true">
      <span className="air-end">app</span>
      <div className="air-track">
        {flying.map((e) => (
          <i
            key={e.id}
            className={`air-pkt tone-${opTone(e)} ${e.dir === "rx" ? "from-board" : "to-board"}`}
          />
        ))}
      </div>
      <span className="air-end">board</span>
    </div>
  );
}
