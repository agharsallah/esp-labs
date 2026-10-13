import { useEffect, useState } from "react";

import { webBluetooth } from "./ble/link.ts";
import { useLink } from "./ble/useLink.ts";
import { ConnectTab } from "./tabs/ConnectTab.tsx";
import { ControlTab } from "./tabs/ControlTab.tsx";
import { LiveTab } from "./tabs/LiveTab.tsx";
import { MusicTab } from "./tabs/MusicTab.tsx";
import { SketchTab } from "./tabs/SketchTab.tsx";
import { SpeedTab } from "./tabs/SpeedTab.tsx";
import { AirStrip } from "./ui/AirStrip.tsx";
import { Unsupported } from "./ui/Unsupported.tsx";

const TABS = [
  { key: "connect", label: "Connect", glyph: "◎", lab: "any", Body: ConnectTab },
  { key: "control", label: "Control", glyph: "◐", lab: "B3+", Body: ControlTab },
  { key: "sketch", label: "Sketch", glyph: "✎", lab: "B5", Body: SketchTab },
  { key: "live", label: "Live", glyph: "∿", lab: "B5", Body: LiveTab },
  { key: "speed", label: "Speed", glyph: "»", lab: "B5", Body: SpeedTab },
  { key: "music", label: "Music", glyph: "♪", lab: "B7", Body: MusicTab },
] as const;

type TabKey = (typeof TABS)[number]["key"];
type Theme = "system" | "light" | "dark";

// Per-viewer conveniences only; the app works the same when storage is blocked.
const store = {
  get: (k: string) => {
    try {
      return localStorage.getItem(k);
    } catch {
      return null;
    }
  },
  set: (k: string, v: string) => {
    try {
      localStorage.setItem(k, v);
    } catch {
      // private mode: fine
    }
  },
};

export function App() {
  const l = useLink();
  const [tab, setTab] = useState<TabKey>(() =>
    TABS.some((t) => t.key === store.get("ble-tab")) ? (store.get("ble-tab") as TabKey) : "connect",
  );
  const [theme, setTheme] = useState<Theme>(() => (store.get("ble-theme") as Theme) || "system");

  useEffect(() => {
    if (theme === "system") delete document.documentElement.dataset.theme;
    else document.documentElement.dataset.theme = theme;
    store.set("ble-theme", theme);
  }, [theme]);
  useEffect(() => store.set("ble-tab", tab), [tab]);

  if (!webBluetooth()) return <Unsupported />;
  const Body = TABS.find((t) => t.key === tab)!.Body;

  return (
    <div className="shell">
      <header className="top">
        <div className="brand">
          <span className={`glyph ${l.status}`} aria-hidden="true" />
          <div>
            <b>Bench radio</b>
            <span className="muted small">Web Bluetooth · no Wi-Fi, no server</span>
          </div>
        </div>
        <div className="status" role="status">
          <span className={`pill ${l.status}`}>
            {l.status === "connected" ? l.deviceName : l.status}
          </span>
          <button
            type="button"
            className="theme"
            onClick={() =>
              setTheme(theme === "system" ? "dark" : theme === "dark" ? "light" : "system")
            }
            aria-label={`Theme: ${theme}. Change theme`}
          >
            {theme === "dark" ? "☾" : theme === "light" ? "☀" : "◑"}
          </button>
        </div>
      </header>
      <AirStrip />
      <nav className="tabs" aria-label="Sections">
        {TABS.map((t) => (
          <button
            key={t.key}
            type="button"
            aria-current={tab === t.key ? "page" : undefined}
            onClick={() => setTab(t.key)}
          >
            <span className="tab-glyph" aria-hidden="true">
              {t.glyph}
            </span>
            <span>{t.label}</span>
            <small>{t.lab}</small>
          </button>
        ))}
      </nav>
      <main className="body">
        <Body />
      </main>
    </div>
  );
}
