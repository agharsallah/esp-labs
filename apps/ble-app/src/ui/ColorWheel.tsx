import { type PointerEvent, useEffect, useRef } from "react";

// A hue/saturation wheel on a canvas. Drag (finger or mouse) or use the arrow keys.
// Pure geometry: colours come from HSV, not from the theme, because they are the LED's colour.
type Props = { hue: number; sat: number; onChange: (hue: number, sat: number) => void };

export function ColorWheel({ hue, sat, onChange }: Props) {
  const ref = useRef<HTMLCanvasElement>(null);
  const size = 220;

  useEffect(() => {
    const g = ref.current?.getContext("2d");
    if (!g) return;
    const r = size / 2;
    const img = g.createImageData(size, size);
    for (let y = 0; y < size; y++)
      for (let x = 0; x < size; x++) {
        const dx = x - r,
          dy = y - r,
          d = Math.hypot(dx, dy) / r;
        const i = (y * size + x) * 4;
        if (d > 1) {
          img.data[i + 3] = 0;
          continue;
        }
        const [R, G, B] = hsvToRgb(((Math.atan2(dy, dx) / (2 * Math.PI)) * 360 + 360) % 360, d, 1);
        ((img.data[i] = R), (img.data[i + 1] = G), (img.data[i + 2] = B), (img.data[i + 3] = 255));
      }
    g.putImageData(img, 0, 0);
  }, []);

  const pick = (e: PointerEvent) => {
    const box = ref.current!.getBoundingClientRect();
    const dx = (e.clientX - box.left) / box.width - 0.5,
      dy = (e.clientY - box.top) / box.height - 0.5;
    onChange(
      ((Math.atan2(dy, dx) / (2 * Math.PI)) * 360 + 360) % 360,
      Math.min(1, Math.hypot(dx, dy) * 2),
    );
  };

  const a = (hue / 180) * Math.PI;
  return (
    <div className="wheel">
      <canvas
        ref={ref}
        width={size}
        height={size}
        role="slider"
        tabIndex={0}
        aria-label="LED colour: hue"
        aria-valuemin={0}
        aria-valuemax={359}
        aria-valuenow={Math.round(hue)}
        aria-valuetext={`hue ${Math.round(hue)} degrees, saturation ${Math.round(sat * 100)} percent`}
        onPointerDown={(e) => {
          e.currentTarget.setPointerCapture(e.pointerId);
          pick(e);
        }}
        onPointerMove={(e) => e.buttons && pick(e)}
        onKeyDown={(e) => {
          const step = e.shiftKey ? 15 : 5;
          if (e.key === "ArrowRight") onChange((hue + step) % 360, sat);
          else if (e.key === "ArrowLeft") onChange((hue - step + 360) % 360, sat);
          else if (e.key === "ArrowUp") onChange(hue, Math.min(1, sat + 0.05));
          else if (e.key === "ArrowDown") onChange(hue, Math.max(0, sat - 0.05));
          else return;
          e.preventDefault();
        }}
      />
      <span
        className="wheel-knob"
        style={{ left: `${50 + Math.cos(a) * sat * 50}%`, top: `${50 + Math.sin(a) * sat * 50}%` }}
      />
    </div>
  );
}

export function hsvToRgb(h: number, s: number, v: number): [number, number, number] {
  const f = (n: number) => {
    const k = (n + h / 60) % 6;
    return Math.round(255 * (v - v * s * Math.max(0, Math.min(k, 4 - k, 1))));
  };
  return [f(5), f(3), f(1)];
}
