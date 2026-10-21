import { useEffect, useMemo, useState } from "react";
import type { Snapshot } from "../../shared/protocol.ts";
import { AdminPanel } from "./components/AdminPanel.tsx";
import { FeedPanel, LedPanel, MessagePanel, ScreenPanel } from "./components/Controls.tsx";
import { LineChart } from "./components/LineChart.tsx";
import { NetworkBars } from "./components/NetworkBars.tsx";
import { useBench } from "./useBench.ts";

type Theme = "system" | "light" | "dark";

function useTheme(): [Theme, () => void] {
  const [theme, setTheme] = useState<Theme>(() => {
    try {
      return (localStorage.getItem("theme") as Theme | null) ?? "system";
    } catch {
      return "system";
    }
  });
  useEffect(() => {
    if (theme === "system") document.documentElement.removeAttribute("data-theme");
    else document.documentElement.dataset.theme = theme;
    try {
      localStorage.setItem("theme", theme);
    } catch {
      // private mode: the choice just isn't remembered
    }
  }, [theme]);
  const next = () =>
    setTheme((t) => (t === "system" ? "light" : t === "light" ? "dark" : "system"));
  return [theme, next];
}

// Link state shown with an icon and words, never colour alone.
function LinkBadge({ serverUp, snapshot }: { serverUp: boolean; snapshot: Snapshot | null }) {
  const wifi = snapshot?.status?.wifi.state;
  const relay = snapshot?.mode === "relay";
  const [kind, icon, text] = !serverUp
    ? ["critical", "✕", "Dashboard server offline"]
    : !snapshot?.espConnected
      ? relay
        ? ["warning", "!", "Waiting for the board to dial in"]
        : ["warning", "!", `Board not reachable at ${snapshot?.boardAt ?? "…"}`]
      : wifi === "connected"
        ? ["good", "●", relay ? "Live · remote" : "Live"]
        : ["warning", "!", `Board Wi-Fi: ${wifi}`];
  return (
    <p className="badge" data-kind={kind} role="status">
      <span aria-hidden="true">{icon}</span> {text}
    </p>
  );
}

function bars(rssi?: number): number {
  if (rssi === undefined) return 0;
  return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
}

function Tiles({ snapshot }: { snapshot: Snapshot | null }) {
  const s = snapshot?.status;
  const rssi = s?.wifi.rssi;
  const tiles: [string, string, string][] = [
    [
      "Signal",
      rssi !== undefined ? `${rssi}` : "—",
      rssi !== undefined
        ? `dBm · ${"▮".repeat(bars(rssi))}${"▯".repeat(4 - bars(rssi))}`
        : "not joined",
    ],
    ["Chip", s?.tempC !== undefined ? s.tempC.toFixed(1) : "—", "°C inside the ESP32"],
    ["Free RAM", s ? `${Math.round(s.heapFree / 1024)}` : "—", "KB internal"],
    [
      "Uptime",
      s
        ? `${Math.floor(s.uptimeS / 3600)}:${String(Math.floor(s.uptimeS / 60) % 60).padStart(2, "0")}`
        : "—",
      "hours:minutes",
    ],
    ["Screen", s?.screen.view ?? "—", s ? `${s.screen.views.length} views` : ""],
  ];
  return (
    <dl className="tiles">
      {tiles.map(([label, value, sub]) => (
        <div key={label}>
          <dt>{label}</dt>
          <dd>
            {value}
            <small>{sub}</small>
          </dd>
        </div>
      ))}
    </dl>
  );
}

export function App({ onSignOut }: { onSignOut?: (() => void) | undefined }) {
  const { serverUp, snapshot, feed, otaPercent } = useBench();
  const [theme, nextTheme] = useTheme();
  const disabled = !snapshot?.espConnected;
  const history = snapshot?.history ?? [];
  const rssi = useMemo(
    () => history.flatMap((p) => (p.rssi === undefined ? [] : [{ t: p.t, v: p.rssi }])),
    [history],
  );
  const temp = useMemo(
    () => history.flatMap((p) => (p.tempC === undefined ? [] : [{ t: p.t, v: p.tempC }])),
    [history],
  );
  const tempDomain: [number, number] = temp.length
    ? [
        Math.floor(Math.min(...temp.map((p) => p.v)) - 2),
        Math.ceil(Math.max(...temp.map((p) => p.v)) + 2),
      ]
    : [20, 60];
  const s = snapshot?.status;

  return (
    <div className="page">
      <header className="top">
        <div>
          <p className="eyebrow">
            esp-labs · wifi-lab-w2 ·{" "}
            {snapshot?.mode === "relay" ? "remote: the board dials in" : "on your Wi-Fi"}
          </p>
          <h1>
            Bench <span>dashboard</span>
          </h1>
          <p className="sub">
            {s?.wifi.state === "connected"
              ? `${s.wifi.ssid} · ${s.wifi.ip} · ${s.wifi.hostname}`
              : "ESP32-S3 N16R8 on your network"}
          </p>
        </div>
        <div className="top-right">
          <LinkBadge serverUp={serverUp} snapshot={snapshot} />
          <button
            type="button"
            className="ghost"
            onClick={nextTheme}
            aria-label={`Theme: ${theme}. Click to change.`}
          >
            Theme: {theme}
          </button>
          {onSignOut && (
            <button type="button" className="ghost" onClick={onSignOut}>
              Sign out
            </button>
          )}
        </div>
      </header>

      <Tiles snapshot={snapshot} />

      <div className="charts">
        <LineChart
          title="Signal strength"
          unit="dBm"
          points={rssi}
          domain={[-95, -30]}
          format={(v) => v.toFixed(0)}
        />
        <LineChart
          title="Chip temperature"
          unit="°C"
          points={temp}
          domain={tempDomain}
          format={(v) => v.toFixed(1)}
        />
      </div>

      <main className="grid">
        <div className="col">
          <ScreenPanel status={s ?? null} disabled={disabled} />
          <MessagePanel status={s ?? null} disabled={disabled} />
          <LedPanel status={s ?? null} disabled={disabled} />
        </div>
        <div className="col">
          <section className="panel" aria-labelledby="nets-h">
            <h2 id="nets-h">
              Networks around the board{" "}
              {snapshot?.networks && (
                <small>
                  {snapshot.networks.networks.length} found in {snapshot.networks.durationMs} ms
                </small>
              )}
            </h2>
            <NetworkBars networks={snapshot?.networks ?? null} />
          </section>
          <FeedPanel feed={feed} />
          <AdminPanel snapshot={snapshot} otaPercent={otaPercent} />
        </div>
      </main>
      <footer className="foot">
        {snapshot?.mode === "relay"
          ? "The browser talks only to the dashboard server; the board keeps a connection open to it from wherever it is, signed in with its admin token."
          : "The browser talks only to the dashboard server; the server talks to the board and holds the admin token. Phone instead? Open http://bench.local."}
      </footer>
    </div>
  );
}
