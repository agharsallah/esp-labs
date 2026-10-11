// Networks around the board as horizontal bars: longer = stronger signal (dBm, -100 to -30).
// Every value is also written out, so the bars are never the only way to read it.
import type { Networks } from "../../../shared/protocol.ts";

const FLOOR = -100;
const CEIL = -30;

export function NetworkBars({ networks }: { networks: Networks | null }) {
  if (!networks) return <p className="empty">No scan yet. Press “Scan now”.</p>;
  if (!networks.networks.length)
    return <p className="empty">The last scan found nothing (2.4 GHz only).</p>;
  return (
    <ul className="bars" aria-label="Networks by signal strength">
      {networks.networks.slice(0, 14).map((n, i) => {
        const pct = ((Math.min(Math.max(n.rssi, FLOOR), CEIL) - FLOOR) / (CEIL - FLOOR)) * 100;
        return (
          <li
            key={`${n.ssid}-${n.channel}-${i}`}
            title={`${n.ssid || "(hidden)"}: ${n.rssi} dBm, channel ${n.channel}, ${n.auth}`}
          >
            <span className={n.ssid ? "name" : "name hidden"}>{n.ssid || "(hidden)"}</span>
            <span className="track">
              <span className="fill" style={{ width: `${pct}%` }} />
            </span>
            <span className="num">{n.rssi} dBm</span>
            <span className="meta">
              ch {n.channel} · {n.auth}
            </span>
          </li>
        );
      })}
    </ul>
  );
}
