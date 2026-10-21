// A single-series time chart: 2px line, faint area, recessive grid, end marker, and a
// crosshair + tooltip on hover or keyboard focus. One measure per chart, never two y-axes.
import { useId, useMemo, useRef, useState, useEffect } from "react";

export interface Point {
  t: number;
  v: number;
}

interface Props {
  title: string;
  unit: string;
  points: Point[];
  domain: [number, number];
  format: (v: number) => string;
  windowMs?: number;
}

const H = 170;
const PAD = { top: 12, right: 14, bottom: 22, left: 40 };

function niceTicks([lo, hi]: [number, number], count = 4): number[] {
  const step = (hi - lo) / count;
  const mag = 10 ** Math.floor(Math.log10(step));
  const nice = [1, 2, 5, 10].map((m) => m * mag).find((s) => s >= step) ?? step;
  const ticks: number[] = [];
  for (let v = Math.ceil(lo / nice) * nice; v <= hi + 1e-9; v += nice)
    ticks.push(Math.round(v * 100) / 100);
  return ticks;
}

export function LineChart({ title, unit, points, domain, format, windowMs = 600_000 }: Props) {
  const box = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(520);
  const [hover, setHover] = useState<number | null>(null);
  const titleId = useId();

  useEffect(() => {
    const el = box.current;
    if (!el) return;
    const ro = new ResizeObserver(
      ([entry]) => entry && setWidth(Math.max(260, entry.contentRect.width)),
    );
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const end = points.at(-1)?.t ?? Date.now();
  const start = end - windowMs;
  const shown = useMemo(() => points.filter((p) => p.t >= start), [points, start]);
  const iw = width - PAD.left - PAD.right;
  const ih = H - PAD.top - PAD.bottom;
  const x = (t: number) => PAD.left + ((t - start) / windowMs) * iw;
  const y = (v: number) =>
    PAD.top +
    (1 - (Math.min(Math.max(v, domain[0]), domain[1]) - domain[0]) / (domain[1] - domain[0])) * ih;
  const line = shown
    .map((p, i) => `${i ? "L" : "M"}${x(p.t).toFixed(1)},${y(p.v).toFixed(1)}`)
    .join("");
  const area =
    shown.length > 1
      ? `${line}L${x(shown.at(-1)!.t).toFixed(1)},${PAD.top + ih}L${x(shown[0]!.t).toFixed(1)},${PAD.top + ih}Z`
      : "";
  const last = shown.at(-1);
  const active = hover !== null ? shown[hover] : undefined;

  function pick(clientX: number) {
    const rect = box.current?.getBoundingClientRect();
    if (!rect || !shown.length) return;
    const t = start + ((clientX - rect.left - PAD.left) / iw) * windowMs;
    let best = 0;
    shown.forEach((p, i) => {
      if (Math.abs(p.t - t) < Math.abs(shown[best]!.t - t)) best = i;
    });
    setHover(best);
  }

  return (
    <figure className="chart">
      <figcaption id={titleId}>
        <span>{title}</span>
        <b>{last ? `${format(last.v)} ${unit}` : "waiting for data"}</b>
      </figcaption>
      <div
        ref={box}
        className="chart-plot"
        tabIndex={0}
        role="img"
        aria-labelledby={titleId}
        onPointerMove={(e) => pick(e.clientX)}
        onPointerLeave={() => setHover(null)}
        onKeyDown={(e) => {
          if (!shown.length) return;
          if (e.key === "ArrowLeft") setHover((h) => Math.max(0, (h ?? shown.length) - 1));
          if (e.key === "ArrowRight")
            setHover((h) => Math.min(shown.length - 1, (h ?? shown.length - 2) + 1));
          if (e.key === "Escape") setHover(null);
        }}
      >
        <svg width={width} height={H} viewBox={`0 0 ${width} ${H}`}>
          {niceTicks(domain).map((v) => (
            <g key={v}>
              <line className="grid" x1={PAD.left} x2={width - PAD.right} y1={y(v)} y2={y(v)} />
              <text className="tick" x={PAD.left - 8} y={y(v) + 4} textAnchor="end">
                {format(v)}
              </text>
            </g>
          ))}
          <text className="tick" x={PAD.left} y={H - 4}>
            −10 min
          </text>
          <text className="tick" x={width - PAD.right} y={H - 4} textAnchor="end">
            now
          </text>
          {area && <path className="area" d={area} />}
          {line && <path className="line" d={line} />}
          {last && !active && <circle className="end" cx={x(last.t)} cy={y(last.v)} r={4} />}
          {active && (
            <g>
              <line
                className="crosshair"
                x1={x(active.t)}
                x2={x(active.t)}
                y1={PAD.top}
                y2={PAD.top + ih}
              />
              <circle className="end" cx={x(active.t)} cy={y(active.v)} r={5} />
            </g>
          )}
        </svg>
        {active && (
          <div
            className="tooltip"
            style={{
              left: Math.min(x(active.t) + 10, width - 150),
              top: Math.max(4, y(active.v) - 44),
            }}
          >
            <b>
              {format(active.v)} {unit}
            </b>
            <span>{new Date(active.t).toLocaleTimeString()}</span>
          </div>
        )}
      </div>
      <details className="table-view">
        <summary>Show as table</summary>
        <table>
          <thead>
            <tr>
              <th>Time</th>
              <th>{unit}</th>
            </tr>
          </thead>
          <tbody>
            {shown
              .slice(-12)
              .reverse()
              .map((p) => (
                <tr key={p.t}>
                  <td>{new Date(p.t).toLocaleTimeString()}</td>
                  <td>{format(p.v)}</td>
                </tr>
              ))}
          </tbody>
        </table>
      </details>
    </figure>
  );
}
