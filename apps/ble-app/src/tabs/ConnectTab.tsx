import { useMemo, useState } from "react";

import type { Dir } from "../ble/link.ts";
import { useLink } from "../ble/useLink.ts";
import { shortUuid } from "../ble/uuids.ts";
import { opTone } from "../ui/ops.ts";

const ARROW: Record<Dir, string> = { tx: "→", rx: "←", info: "·", error: "!" };

export function ConnectTab() {
  const l = useLink();
  const [filter, setFilter] = useState<"all" | "tx" | "rx">("all");
  const log = useMemo(
    () => [...l.log].reverse().filter((e) => filter === "all" || e.dir === filter),
    [l.log, filter],
  );
  const busy = l.status === "choosing" || l.status === "connecting";

  return (
    <div className="grid two">
      <section className="card hero">
        <div className={`beacon ${l.status}`} aria-hidden="true">
          <i />
          <i />
          <i />
          <b />
        </div>
        <div>
          <p className="eyebrow">
            {l.status === "connected"
              ? "connected"
              : l.status === "lost"
                ? "connection lost"
                : "not connected"}
          </p>
          <h2>{l.status === "connected" ? l.deviceName : "Find your bench board"}</h2>
          <p className="muted">
            {l.status === "connected"
              ? "A GATT client in your browser, talking to the board's GATT server. Everything below is live."
              : "The browser shows a list of nearby boards running B3 to B7 (or any BLE-MIDI instrument). Pick one. A page can never connect without you choosing."}
          </p>
          <div className="row">
            {l.status === "connected" ? (
              <button type="button" className="btn" onClick={() => l.disconnect()}>
                Disconnect
              </button>
            ) : (
              <button
                type="button"
                className="btn primary"
                onClick={() => l.connect()}
                disabled={busy}
              >
                {busy
                  ? l.status === "choosing"
                    ? "Choosing…"
                    : "Connecting…"
                  : l.status === "lost"
                    ? "Reconnect"
                    : "Connect"}
              </button>
            )}
          </div>
          {l.error && <p className="error">{l.error}</p>}
        </div>
      </section>

      <section className="card">
        <h3>GATT explorer</h3>
        {l.services.length === 0 ? (
          <p className="muted">
            Services and characteristics appear here after connecting: the board's little database.
          </p>
        ) : (
          <ul className="tree">
            {l.services.map((s) => (
              <li key={s.uuid}>
                <div className="svc">
                  <b>{s.name ?? "Unknown service"}</b>
                  <code>{shortUuid(s.uuid)}</code>
                </div>
                <ul>
                  {s.chars.map((c) => (
                    <li key={c.uuid} className={c.name ? "known" : ""}>
                      <span>{c.name ?? "characteristic"}</span>
                      <code>{shortUuid(c.uuid)}</code>
                      <span className="props">
                        {c.props.map((p) => (
                          <em key={p}>{p}</em>
                        ))}
                      </span>
                    </li>
                  ))}
                </ul>
              </li>
            ))}
          </ul>
        )}
      </section>

      <section className="card span">
        <div className="row between">
          <h3>On the air, byte by byte</h3>
          <div className="seg" role="group" aria-label="Filter the log">
            {(["all", "tx", "rx"] as const).map((f) => (
              <button
                key={f}
                type="button"
                aria-pressed={filter === f}
                onClick={() => setFilter(f)}
              >
                {f === "all" ? "all" : f === "tx" ? "sent →" : "← received"}
              </button>
            ))}
          </div>
        </div>
        <div className="log" role="log" aria-live="polite">
          {log.length === 0 && <p className="muted">Nothing yet.</p>}
          {log.map((e) => (
            <div key={e.id} className={`log-row tone-${opTone(e)}`}>
              <time>{(e.at / 1000).toFixed(2)}</time>
              <span className="arrow">{ARROW[e.dir]}</span>
              <span className="op">{e.op}</span>
              <span className="what">{e.what}</span>
              <code>{e.bytes}</code>
            </div>
          ))}
        </div>
      </section>
    </div>
  );
}
