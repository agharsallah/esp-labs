import { useEffect, useRef, useState } from "react";

import { link } from "../ble/link.ts";
import { useLink, useNotify } from "../ble/useLink.ts";
import { type ButtonEvent, decodeButton, decodeTemperature } from "../protocol/sensors.ts";
import { ColorWheel, hsvToRgb } from "../ui/ColorWheel.tsx";
import { LineChart } from "../ui/LineChart.tsx";
import { Needs } from "../ui/Needs.tsx";

const encoder = new TextEncoder();

// The first things B3 taught, now with real controls: LED, message, button, temperature.
export function ControlTab() {
  return (
    <Needs chr="led" lab="B3, B4 or B5 (the Bench service)">
      <Controls />
    </Needs>
  );
}

function Controls() {
  const l = useLink();
  const [hue, setHue] = useState(210);
  const [sat, setSat] = useState(0.8);
  const [bright, setBright] = useState(0.6);
  const [message, setMessage] = useState("Hello from the app");
  const [events, setEvents] = useState<(ButtonEvent & { at: number })[]>([]);
  const [temps, setTemps] = useState<number[]>([]);
  const sendAt = useRef(0);
  const pendingLed = useRef<number | null>(null);
  const touched = useRef(false); // only send once you've moved a control, not on opening the tab

  const rgb = hsvToRgb(hue, sat, bright);
  const bytes = encoder.encode(message);

  // LED writes are throttled to one per 80 ms: a drag on the wheel fires far more often than the radio can send.
  useEffect(() => {
    if (!touched.current) return;
    const send = () => {
      sendAt.current = performance.now();
      pendingLed.current = null;
      void link.write("led", Uint8Array.from(rgb));
    };
    const wait = 80 - (performance.now() - sendAt.current);
    if (wait <= 0) send();
    else {
      if (pendingLed.current) clearTimeout(pendingLed.current);
      pendingLed.current = window.setTimeout(send, wait);
    }
    return () => {
      if (pendingLed.current) clearTimeout(pendingLed.current);
    };
    // rgb is derived from these three.
  }, [hue, sat, bright]);

  useNotify("button", (v) => {
    const e = decodeButton(v);
    if (e) setEvents((list) => [{ ...e, at: Date.now() }, ...list].slice(0, 8));
  });
  useNotify("temperature", (v) => {
    const t = decodeTemperature(v);
    if (t !== null) setTemps((list) => [...list, t].slice(-90));
  });
  useEffect(() => {
    void link.read("temperature").then((v) => {
      const t = v && decodeTemperature(v);
      if (t !== null && t !== undefined) setTemps((list) => (list.length ? list : [t]));
    });
  }, []);

  return (
    <div className="grid two">
      <section className="card">
        <h3>LED colour</h3>
        <div className="led-row">
          <ColorWheel
            hue={hue}
            sat={sat}
            onChange={(h, s) => ((touched.current = true), setHue(h), setSat(s))}
          />
          <div className="led-side">
            <div
              className="led-orb"
              style={{
                background: `rgb(${rgb.join(" ")})`,
                boxShadow: `0 0 40px 6px rgb(${rgb.join(" ")} / ${bright * 0.7})`,
              }}
              aria-hidden="true"
            />
            <label className="field">
              <span>Brightness</span>
              <input
                type="range"
                min="0"
                max="1"
                step="0.01"
                value={bright}
                onChange={(e) => ((touched.current = true), setBright(Number(e.target.value)))}
              />
            </label>
            <code className="bytes" aria-label="Bytes written">
              write LED{" "}
              <b>{rgb.map((v) => v.toString(16).toUpperCase().padStart(2, "0")).join(" ")}</b>
            </code>
          </div>
        </div>
      </section>

      <section className="card">
        <h3>Message for the screen</h3>
        <form
          className="msg"
          onSubmit={(e) => {
            e.preventDefault();
            void link.write("message", bytes.slice(0, 40));
          }}
        >
          <input
            value={message}
            maxLength={60}
            onChange={(e) => setMessage(e.target.value)}
            aria-label="Message"
          />
          <button type="submit" className="btn primary" disabled={l.status !== "connected"}>
            Send
          </button>
        </form>
        <div
          className={`meter ${bytes.length > 40 ? "over" : ""}`}
          aria-label={`${bytes.length} of 40 bytes`}
        >
          <i style={{ width: `${Math.min(100, (bytes.length / 40) * 100)}%` }} />
        </div>
        <p className="muted small">
          {bytes.length} of 40 bytes as UTF-8{bytes.length > 40 ? ": the end is cut off" : ""}. An
          emoji costs 4 bytes, and the screen's font can't draw it.
        </p>

        <h3>Button</h3>
        <p className="muted small">
          Press the push button or BOOT: each gesture arrives as a notification (the board pushes,
          the app never asks).
        </p>
        <ol className="events">
          {events.length === 0 && <li className="muted">Nothing pressed yet.</li>}
          {events.map((e) => (
            <li key={e.at + e.count} className={`ev ev-${e.gesture}`}>
              <b>
                {e.gesture === "double"
                  ? "double-click"
                  : e.gesture === "long"
                    ? "long press"
                    : "click"}
              </b>
              <span>#{e.count}</span>
            </li>
          ))}
        </ol>
      </section>

      <section className="card span">
        <LineChart
          values={temps}
          tone="sub"
          label="Chip temperature (notified every 2 s)"
          unit=" °C"
          digits={2}
        />
      </section>
    </div>
  );
}
