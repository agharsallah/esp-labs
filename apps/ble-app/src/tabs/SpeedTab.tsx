import { useRef, useState } from "react";

import { link } from "../ble/link.ts";
import { useNotify } from "../ble/useLink.ts";
import {
  LINK_OPS,
  type SpeedSummary,
  decodeSpeed,
  startSpeed,
  stopSpeed,
} from "../protocol/speed.ts";
import { Needs } from "../ui/Needs.tsx";

type Run = {
  bytes: number;
  appMs: number;
  packets: number;
  outOfOrder: number;
  board: SpeedSummary | null;
};

const SIZES = [20_000, 50_000, 100_000, 200_000];
const PHY = ["?", "1M", "2M", "Coded"];

export function SpeedTab() {
  return (
    <Needs chr="speed" lab="B5">
      <Speed />
    </Needs>
  );
}

function Speed() {
  const [size, setSize] = useState(50_000);
  const [runs, setRuns] = useState<Run[]>([]);
  const [live, setLive] = useState<{ bytes: number; packets: number } | null>(null);
  const cur = useRef<{
    first: number;
    last: number;
    bytes: number;
    packets: number;
    seq: number;
    outOfOrder: number;
  } | null>(null);

  useNotify("speed", (v) => {
    const p = decodeSpeed(v);
    if (!p) return;
    const now = performance.now();
    if (p.kind === "data") {
      const c = (cur.current ??= {
        first: now,
        last: now,
        bytes: 0,
        packets: 0,
        seq: p.seq - 1,
        outOfOrder: 0,
      });
      if (p.seq !== ((c.seq + 1) & 0xffff) && !(c.seq === 0xfffe && p.seq === 0)) c.outOfOrder++;
      c.seq = p.seq;
      c.bytes += p.size;
      c.packets++;
      c.last = now;
      if (c.packets % 20 === 0) setLive({ bytes: c.bytes, packets: c.packets });
      return;
    }
    const c = cur.current;
    cur.current = null;
    setLive(null);
    setRuns((r) =>
      [
        ...r,
        {
          bytes: c?.bytes ?? 0,
          appMs: c ? Math.max(1, c.last - c.first) : 1,
          packets: c?.packets ?? 0,
          outOfOrder: c?.outOfOrder ?? 0,
          board: p.summary,
        },
      ].slice(-8),
    );
    link.note("rx", "notify", `Speed summary: ${p.summary.bytes} B in ${p.summary.ms} ms`);
  });

  const peak = Math.max(1, ...runs.map((r) => r.bytes / r.appMs));
  const last = runs[runs.length - 1];
  const payload = last?.board ? last.board.mtu - 3 : 20;
  const interval = last?.board?.intervalMs ?? 30;

  return (
    <div className="grid two">
      <section className="card">
        <h3>Run a speed test</h3>
        <div className="row wrap">
          <div className="seg" role="group" aria-label="How many bytes">
            {SIZES.map((s) => (
              <button key={s} type="button" aria-pressed={size === s} onClick={() => setSize(s)}>
                {s / 1000} kB
              </button>
            ))}
          </div>
          <button
            type="button"
            className="btn primary"
            disabled={!!live}
            onClick={() => {
              cur.current = null;
              setLive({ bytes: 0, packets: 0 });
              void link.write("speed", startSpeed(size));
            }}
          >
            {live ? "Receiving…" : "Run"}
          </button>
          {live && (
            <button
              type="button"
              className="btn"
              onClick={() => void link.write("speed", stopSpeed())}
            >
              Stop
            </button>
          )}
        </div>
        {live && (
          <div className="meter tall" aria-label={`${live.bytes} of ${size} bytes`}>
            <i style={{ width: `${(live.bytes / size) * 100}%` }} />
          </div>
        )}
        <h3>Change the link, then run again</h3>
        <div className="row wrap">
          <button
            type="button"
            className="btn"
            onClick={() => void link.write("link", Uint8Array.of(LINK_OPS.fast))}
          >
            Fast interval
          </button>
          <button
            type="button"
            className="btn"
            onClick={() => void link.write("link", Uint8Array.of(LINK_OPS.slow))}
          >
            Slow interval
          </button>
          <button
            type="button"
            className="btn"
            onClick={() => void link.write("link", Uint8Array.of(LINK_OPS.phy2m))}
          >
            2M PHY
          </button>
          <button
            type="button"
            className="btn"
            onClick={() => void link.write("link", Uint8Array.of(LINK_OPS.phy1m))}
          >
            1M PHY
          </button>
          <button
            type="button"
            className="btn"
            onClick={() => void link.write("link", Uint8Array.of(LINK_OPS.dle))}
          >
            Long packets
          </button>
        </div>
        <p className="muted small">
          The board only asks. Your phone or Mac (the central) decides; the board's link view shows
          what it accepted.
        </p>
      </section>

      <section className="card">
        <h3>Results (kB/s measured in the app)</h3>
        {runs.length === 0 && <p className="muted">No runs yet.</p>}
        <ol className="bars">
          {runs.map((r, i) => {
            const k = r.bytes / r.appMs;
            return (
              <li key={i}>
                <span className="bar" style={{ width: `${(k / peak) * 100}%` }} />
                <b>{k.toFixed(1)} kB/s</b>
                <span className="muted small">
                  {r.board
                    ? `MTU ${r.board.mtu} · ${r.board.intervalMs} ms · ${PHY[r.board.phy & 3]} · board ${(r.board.bytes / r.board.ms).toFixed(1)}`
                    : "no summary"}
                  {r.outOfOrder ? ` · ${r.outOfOrder} gaps` : ""}
                </span>
              </li>
            );
          })}
        </ol>
      </section>

      <section className="card span intro">
        <h3>Why it's this fast, and no faster</h3>
        <p className="formula">
          kB/s ≈ <b>{payload} B</b> per packet × <b>packets per event</b> ÷ <b>{interval} ms</b> per
          event
        </p>
        <p>
          The radio talks only in connection events, one every interval. In each event the board can
          send a few notifications of up to MTU - 3 bytes. A bigger MTU (fewer headers), a shorter
          interval (more events), long packets and the 2M PHY (less time on air) all raise it.
          Phones cap packets per event: that is why the same board is faster with the Mac than with
          some phones.
        </p>
      </section>
    </div>
  );
}
