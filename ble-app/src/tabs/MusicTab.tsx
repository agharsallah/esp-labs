import { useEffect, useRef, useState } from "react";

import { Synth } from "../audio/synth.ts";
import { link } from "../ble/link.ts";
import { useNotify } from "../ble/useLink.ts";
import { decode, noteName, noteOff, noteOn } from "../protocol/midi.ts";
import { Needs } from "../ui/Needs.tsx";

const LOW = 60; // C4
const KEYS = 25; // two octaves and the top C
const COMPUTER_KEYS = "awsedftgyhujkolp;'"; // a = C4, w = C#4, s = D4 …
const isBlack = (n: number) => [1, 3, 6, 8, 10].includes(n % 12);

type Hit = { note: number; from: "app" | "board"; start: number; end: number | null };

export function MusicTab() {
  return (
    <Needs chr="midi" lab="B7 (or any BLE-MIDI instrument)">
      <Music />
    </Needs>
  );
}

const synth = new Synth();

function Music() {
  const [down, setDown] = useState<Map<number, "app" | "board">>(new Map());
  const [local, setLocal] = useState(true);
  const [volume, setVolume] = useState(0.5);
  const hits = useRef<Hit[]>([]);
  const roll = useRef<HTMLCanvasElement>(null);

  const mark = (note: number, from: "app" | "board", on: boolean) => {
    setDown((d) => {
      const m = new Map(d);
      if (on) m.set(note, from);
      else m.delete(note);
      return m;
    });
    const now = performance.now();
    if (on) hits.current = [...hits.current.slice(-200), { note, from, start: now, end: null }];
    else for (const h of hits.current) if (h.note === note && h.end === null) h.end = now;
  };

  // From the board: play it here and draw it on the roll.
  useNotify("midi", (v) => {
    for (const m of decode(new Uint8Array(v.buffer, v.byteOffset, v.byteLength))) {
      if (m.type === "noteOn") {
        synth.noteOn(m.note, m.velocity);
        mark(m.note, "board", true);
      } else if (m.type === "noteOff") {
        synth.noteOff(m.note);
        mark(m.note, "board", false);
      }
    }
  });

  const play = (note: number, on: boolean) => {
    if (on) {
      if (local) synth.noteOn(note, 100);
      void link.write("midi", noteOn(note, 100), false);
    } else {
      synth.noteOff(note);
      void link.write("midi", noteOff(note), false);
    }
    mark(note, "app", on);
  };

  // Computer keyboard as a piano.
  useEffect(() => {
    const held = new Set<string>();
    const onKey = (e: KeyboardEvent, on: boolean) => {
      if (e.target instanceof HTMLInputElement || e.metaKey || e.ctrlKey) return;
      const i = COMPUTER_KEYS.indexOf(e.key.toLowerCase());
      if (i < 0) return;
      if (on === held.has(e.key)) return; // key repeat
      on ? held.add(e.key) : held.delete(e.key);
      play(LOW + i, on);
    };
    const d = (e: KeyboardEvent) => onKey(e, true);
    const u = (e: KeyboardEvent) => onKey(e, false);
    addEventListener("keydown", d);
    addEventListener("keyup", u);
    return () => {
      removeEventListener("keydown", d);
      removeEventListener("keyup", u);
    };
  });

  useEffect(() => synth.setVolume(volume), [volume]);

  // The piano roll: time flows left, newest notes enter on the right. 4 seconds wide.
  useEffect(() => {
    const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
    let raf = 0;
    const draw = () => {
      const c = roll.current;
      const g = c?.getContext("2d");
      if (c && g) {
        const css = getComputedStyle(c);
        const W = c.width,
          H = c.height,
          now = performance.now(),
          span = 4000;
        g.clearRect(0, 0, W, H);
        g.fillStyle = css.getPropertyValue("--grid");
        for (let n = 0; n < KEYS; n++)
          if (isBlack(LOW + n)) g.fillRect(0, H - ((n + 1) * H) / KEYS, W, H / KEYS);
        for (const h of hits.current) {
          const x0 = W - ((now - h.start) / span) * W;
          const x1 = h.end === null ? W : W - ((now - h.end) / span) * W;
          if (x1 < 0) continue;
          g.fillStyle = css.getPropertyValue(h.from === "board" ? "--notify" : "--radio");
          g.fillRect(
            x0,
            H - ((h.note - LOW + 1) * H) / KEYS + 1,
            Math.max(3, x1 - x0),
            H / KEYS - 2,
          );
        }
      }
      if (!reduce) raf = requestAnimationFrame(draw);
    };
    draw();
    const slow = reduce ? setInterval(draw, 250) : 0;
    return () => (cancelAnimationFrame(raf), clearInterval(slow));
  }, []);

  const whites = Array.from({ length: KEYS }, (_, i) => LOW + i).filter((n) => !isBlack(n));
  return (
    <div className="grid">
      <section className="card">
        <div className="row between wrap">
          <p className="legend">
            <span>
              <span className="dot tone-write" /> you play (sent to the board)
            </span>
            <span>
              <span className="dot tone-notify" /> the board plays (notified)
            </span>
          </p>
          <label className="field inline">
            <input type="checkbox" checked={local} onChange={(e) => setLocal(e.target.checked)} />{" "}
            <span>sound here too</span>
          </label>
          <label className="field inline">
            <span>Volume</span>
            <input
              type="range"
              min="0"
              max="1"
              step="0.05"
              value={volume}
              onChange={(e) => setVolume(Number(e.target.value))}
            />
          </label>
        </div>
        <canvas
          ref={roll}
          className="roll"
          width={640}
          height={150}
          aria-label="Piano roll of the last 4 seconds"
        />
        <div
          className="piano"
          role="group"
          aria-label="Piano, C4 to C6. Computer keys A W S E D F T G Y H U J K play too."
        >
          {whites.map((n) => {
            const black = isBlack(n + 1) && n + 1 < LOW + KEYS ? n + 1 : null;
            return (
              <div key={n} className="key-slot">
                <Key note={n} state={down.get(n)} onPlay={play} />
                {black !== null && <Key note={black} state={down.get(black)} onPlay={play} black />}
              </div>
            );
          })}
        </div>
      </section>
      <section className="card intro">
        <h3>What one note looks like on the air</h3>
        <p>
          Middle C pressed: <code>87 E8 90 3C 64</code>. A header with the time stamp's high bits (
          <code>87</code>), the low bits (<code>E8</code>), then plain MIDI: note on, channel 1 (
          <code>90</code>), note 60 (<code>3C</code>), velocity 100 (<code>64</code>). Sent as a
          write without response, so a chord is a few bytes in one connection event.
        </p>
      </section>
    </div>
  );
}

function Key({
  note,
  state,
  onPlay,
  black = false,
}: {
  note: number;
  state?: "app" | "board";
  onPlay: (n: number, on: boolean) => void;
  black?: boolean;
}) {
  return (
    <button
      type="button"
      className={`key ${black ? "black" : "white"} ${state ? `lit lit-${state}` : ""}`}
      aria-label={noteName(note)}
      aria-pressed={!!state}
      onPointerDown={(e) => {
        e.currentTarget.setPointerCapture(e.pointerId);
        onPlay(note, true);
      }}
      onPointerUp={() => onPlay(note, false)}
      onPointerCancel={() => onPlay(note, false)}
      onKeyDown={(e) =>
        (e.key === "Enter" || e.key === " ") &&
        !e.repeat &&
        (e.preventDefault(), onPlay(note, true))
      }
      onKeyUp={(e) => (e.key === "Enter" || e.key === " ") && onPlay(note, false)}
    >
      {!black && note % 12 === 0 && <span>{noteName(note)}</span>}
    </button>
  );
}
