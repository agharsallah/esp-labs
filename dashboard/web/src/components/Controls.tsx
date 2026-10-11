// Everyday controls: what the screen shows, a message, the on-board LED. None need the admin token.
import { useState } from "react";
import type { Status } from "../../../shared/protocol.ts";
import { send, type Result } from "../api.ts";
import type { FeedItem } from "../useBench.ts";

function useAction() {
  const [error, setError] = useState<string | null>(null);
  const run = async (p: Promise<Result>) => {
    const r = await p;
    setError(r.ok ? null : r.error);
  };
  return { error, run };
}

export function ScreenPanel({ status, disabled }: { status: Status | null; disabled: boolean }) {
  const { error, run } = useAction();
  const screen = status?.screen;
  return (
    <section className="panel" aria-labelledby="screen-h">
      <h2 id="screen-h">Screen</h2>
      <div className="segmented" role="radiogroup" aria-label="View on the board's screen">
        {(screen?.views ?? ["networks", "channels", "radar", "status", "message"]).map((v) => (
          <button
            key={v}
            type="button"
            role="radio"
            aria-checked={screen?.view === v}
            disabled={disabled}
            onClick={() => run(send("PUT", "/api/view", { view: v }))}
          >
            {v}
          </button>
        ))}
      </div>
      <div className="row">
        <label className="switch">
          <input
            type="checkbox"
            checked={screen?.autoScan ?? false}
            disabled={disabled}
            onChange={(e) => run(send("PUT", "/api/settings", { autoScan: e.target.checked }))}
          />
          Auto-scan (every 10 s, 60 s once joined)
        </label>
        <button
          type="button"
          disabled={disabled || status?.scan.running}
          onClick={() => run(send("POST", "/api/scan"))}
        >
          {status?.scan.running ? "Scanning…" : "Scan now"}
        </button>
      </div>
      <label className="slider" htmlFor="backlight">
        Brightness <output>{screen ? Math.round((screen.backlight / 255) * 100) : 0}%</output>
      </label>
      <input
        id="backlight"
        type="range"
        min={0}
        max={255}
        disabled={disabled}
        key={screen?.backlight}
        defaultValue={screen?.backlight ?? 255}
        onPointerUp={(e) =>
          run(send("PUT", "/api/settings", { backlight: Number(e.currentTarget.value) }))
        }
        onKeyUp={(e) =>
          run(send("PUT", "/api/settings", { backlight: Number(e.currentTarget.value) }))
        }
      />
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}

const SUGGESTIONS = [
  "Hello from the dashboard",
  "Coffee break ☕",
  "Build passing",
  "Back in 5 min",
];

export function MessagePanel({ status, disabled }: { status: Status | null; disabled: boolean }) {
  const { error, run } = useAction();
  const [text, setText] = useState("");
  const show = (t: string) => run(send("PUT", "/api/message", { text: t }));
  return (
    <section className="panel" aria-labelledby="msg-h">
      <h2 id="msg-h">Message on the screen</h2>
      <form
        className="row"
        onSubmit={(e) => {
          e.preventDefault();
          void show(text);
        }}
      >
        <input
          id="message"
          aria-label="Message"
          maxLength={96}
          value={text}
          placeholder="Type up to 96 characters"
          onChange={(e) => setText(e.target.value)}
        />
        <button type="submit" disabled={disabled}>
          Show
        </button>
      </form>
      <div className="chips">
        {SUGGESTIONS.map((s) => (
          <button
            key={s}
            type="button"
            className="chip"
            disabled={disabled}
            onClick={() => show(s.replace(" ☕", ""))}
          >
            {s}
          </button>
        ))}
      </div>
      <p className="muted">
        On screen now: {status?.screen.message ? `“${status.screen.message}”` : "nothing"}
      </p>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}

// LED colours are data sent to the board (0-255 per channel), not page colours.
const PRESETS: [string, number, number, number][] = [
  ["Teal", 0, 160, 120],
  ["Amber", 255, 120, 0],
  ["Violet", 120, 0, 255],
  ["White", 120, 120, 120],
];

export function LedPanel({ status, disabled }: { status: Status | null; disabled: boolean }) {
  const { error, run } = useAction();
  const led = status?.led ?? { r: 0, g: 0, b: 0 };
  const hex = `#${[led.r, led.g, led.b].map((c) => c.toString(16).padStart(2, "0")).join("")}`;
  const set = (r: number, g: number, b: number) => run(send("PUT", "/api/led", { r, g, b }));
  return (
    <section className="panel" aria-labelledby="led-h">
      <h2 id="led-h">On-board LED</h2>
      <div className="row">
        <span
          className="led"
          style={{ background: `rgb(${led.r} ${led.g} ${led.b})` }}
          aria-hidden="true"
        />
        <input
          type="color"
          aria-label="LED colour"
          value={hex}
          disabled={disabled}
          onChange={(e) => {
            const v = e.target.value;
            void set(
              parseInt(v.slice(1, 3), 16),
              parseInt(v.slice(3, 5), 16),
              parseInt(v.slice(5, 7), 16),
            );
          }}
        />
        {PRESETS.map(([name, r, g, b]) => (
          <button
            key={name}
            type="button"
            className="chip"
            disabled={disabled}
            onClick={() => set(r, g, b)}
          >
            {name}
          </button>
        ))}
        <button type="button" className="chip" disabled={disabled} onClick={() => set(0, 0, 0)}>
          Off
        </button>
      </div>
      <p className="muted">
        Now: rgb({led.r}, {led.g}, {led.b}). The LED is very bright; values under 60 are easier on
        the eyes.
      </p>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}

export function FeedPanel({ feed }: { feed: FeedItem[] }) {
  return (
    <section className="panel" aria-labelledby="feed-h">
      <h2 id="feed-h">Live events</h2>
      {feed.length === 0 ? (
        <p className="empty">
          Press the button on the board: clicks, double-clicks and long presses show up here.
        </p>
      ) : (
        <ol className="feed" aria-live="polite">
          {feed.map((f) => (
            <li key={f.id} data-kind={f.kind}>
              <time>{f.time.toLocaleTimeString()}</time>
              <span>{f.text}</span>
            </li>
          ))}
        </ol>
      )}
    </section>
  );
}
