import { type PointerEvent, useEffect, useRef, useState } from "react";

import { link } from "../ble/link.ts";
import { useLink } from "../ble/useLink.ts";
import { AREA, type Point, clear, dot, rgb565, strokeChunks, text } from "../protocol/canvas.ts";
import { Needs } from "../ui/Needs.tsx";

// Pen colours: picked to read well on the board's dark screen. They are the bytes we send, so
// they live here as data, not as theme tokens.
const INKS = [
  { name: "white", rgb: [242, 244, 248] },
  { name: "teal", rgb: [70, 220, 190] },
  { name: "blue", rgb: [80, 150, 255] },
  { name: "violet", rgb: [176, 130, 255] },
  { name: "amber", rgb: [255, 176, 60] },
  { name: "coral", rgb: [255, 96, 84] },
  { name: "green", rgb: [120, 255, 140] },
].map((c) => ({ ...c, value: `rgb(${c.rgb.join(" ")})` }));
const NIGHT = [10, 14, 22] as const; // the board's own background colour (pal::night)
const BACKGROUND = `rgb(${NIGHT.join(" ")})`;

export function SketchTab() {
  return (
    <Needs chr="canvas" lab="B5">
      <Sketch />
    </Needs>
  );
}

function Sketch() {
  const l = useLink();
  const canvas = useRef<HTMLCanvasElement>(null);
  const [ink, setInk] = useState(INKS[1]!.value);
  const [width, setWidth] = useState(4);
  const [tool, setTool] = useState<"pen" | "dot" | "text">("pen");
  const [words, setWords] = useState("hi bench");
  const [sent, setSent] = useState({ writes: 0, bytes: 0 });
  const pending = useRef<Point[]>([]);
  const lastSent = useRef<Point | null>(null);
  const drawing = useRef(false);

  const colour = () => {
    const [r = 0, g = 0, b = 0] = INKS.find((c) => c.value === ink)?.rgb ?? [];
    return rgb565(r, g, b);
  };

  const send = async (bytes: Uint8Array) => {
    // Write without response: no answer per write, so many fit in one connection event.
    const ok = await link.write("canvas", bytes, false, true);
    if (ok) setSent((s) => ({ writes: s.writes + 1, bytes: s.bytes + bytes.length }));
    return ok;
  };

  // Every 50 ms, send the points drawn since the last flush, as strokes that fit one write each.
  useEffect(() => {
    const id = setInterval(async () => {
      if (pending.current.length === 0 || link.backlog > 4) return; // the radio is behind: wait
      const pts = lastSent.current ? [lastSent.current, ...pending.current] : pending.current;
      lastSent.current = pending.current[pending.current.length - 1] ?? null;
      pending.current = [];
      if (!drawing.current) lastSent.current = null;
      for (const chunk of strokeChunks(colour(), width, pts, link.maxWrite)) {
        if (!(await send(chunk)) && link.maxWrite === 20)
          for (const small of strokeChunks(colour(), width, pts, 20)) await send(small);
      }
    }, 50);
    return () => clearInterval(id);
  });

  const toArea = (e: PointerEvent<HTMLCanvasElement>): Point => {
    const box = e.currentTarget.getBoundingClientRect();
    return {
      x: ((e.clientX - box.left) / box.width) * AREA.w,
      y: ((e.clientY - box.top) / box.height) * AREA.h,
    };
  };

  const paint = (draw: (g: CanvasRenderingContext2D) => void) => {
    const g = canvas.current?.getContext("2d");
    if (g) draw(g);
  };

  const wipe = () => {
    paint((g) => {
      g.fillStyle = BACKGROUND;
      g.fillRect(0, 0, AREA.w, AREA.h);
    });
    void send(clear(rgb565(...NIGHT)));
  };

  useEffect(() => {
    paint((g) => {
      g.fillStyle = BACKGROUND;
      g.fillRect(0, 0, AREA.w, AREA.h);
    });
  }, []);

  return (
    <div className="grid">
      <section className="card">
        <div className="row between wrap">
          <div className="seg" role="group" aria-label="Tool">
            {(["pen", "dot", "text"] as const).map((t) => (
              <button key={t} type="button" aria-pressed={tool === t} onClick={() => setTool(t)}>
                {t}
              </button>
            ))}
          </div>
          <div className="inks" role="radiogroup" aria-label="Ink">
            {INKS.map((c) => (
              <button
                key={c.value}
                type="button"
                role="radio"
                aria-checked={ink === c.value}
                aria-label={c.name}
                className="ink"
                style={{ background: c.value }}
                onClick={() => setInk(c.value)}
              />
            ))}
          </div>
          <label className="field inline">
            <span>Width {width}</span>
            <input
              type="range"
              min="1"
              max="16"
              value={width}
              onChange={(e) => setWidth(Number(e.target.value))}
            />
          </label>
          <button type="button" className="btn" onClick={wipe}>
            Clear both
          </button>
        </div>
        {tool === "text" && (
          <label className="field">
            <span>Text to place (tap the canvas)</span>
            <input value={words} maxLength={40} onChange={(e) => setWords(e.target.value)} />
          </label>
        )}
        <div className="sketch-frame">
          <canvas
            ref={canvas}
            width={AREA.w}
            height={AREA.h}
            className="sketch"
            aria-label="Sketch pad, mirrored on the board's screen"
            onPointerDown={(e) => {
              e.currentTarget.setPointerCapture(e.pointerId);
              const p = toArea(e);
              if (tool === "dot") {
                paint((g) => {
                  g.fillStyle = ink;
                  g.beginPath();
                  g.arc(p.x, p.y, width, 0, Math.PI * 2);
                  g.fill();
                });
                return void send(dot(colour(), p, width));
              }
              if (tool === "text") {
                paint((g) => {
                  g.fillStyle = ink;
                  g.font = "16px monospace";
                  g.textBaseline = "top";
                  g.fillText(words, p.x, p.y);
                });
                return void send(text(colour(), p, 2, words));
              }
              drawing.current = true;
              lastSent.current = null;
              pending.current = [p];
            }}
            onPointerMove={(e) => {
              if (!drawing.current) return;
              const p = toArea(e);
              const prev = pending.current[pending.current.length - 1] ?? lastSent.current;
              if (prev && Math.hypot(p.x - prev.x, p.y - prev.y) < 2) return; // skip tiny moves: fewer bytes
              paint((g) => {
                g.strokeStyle = ink;
                g.lineWidth = width;
                g.lineCap = "round";
                g.beginPath();
                if (prev) g.moveTo(prev.x, prev.y);
                g.lineTo(p.x, p.y);
                g.stroke();
              });
              pending.current.push(p);
            }}
            onPointerUp={() => (drawing.current = false)}
            onPointerCancel={() => (drawing.current = false)}
          />
        </div>
        <p className="muted small">
          {sent.writes} writes · {sent.bytes} bytes · up to {link.maxWrite} bytes per write (
          {Math.floor((link.maxWrite - 5) / 3)} points)
          {l.status === "connected" ? "" : " · not connected"}
        </p>
      </section>
      <section className="card intro">
        <h3>What crosses the air</h3>
        <p>
          A stroke is sent as <code>02 cc cc w n</code> and then 3 bytes per point (x low, x high,
          y): about 20 points per stroke chunk every 50 ms. Each one is a{" "}
          <b>write without response</b>: the board doesn't answer, so several fit in one connection
          event. If the radio falls behind, the app waits instead of piling up writes.
        </p>
      </section>
    </div>
  );
}
