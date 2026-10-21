// The sketch protocol, app side. The board's half (and the table) is labs/ble-lab-b5/src/canvas_protocol.h:
//   01 cc cc                       clear
//   02 cc cc w n [xx xx yy] x n    stroke, n = 1-80 points
//   03 cc cc xx xx yy ww ww hh     rect
//   04 cc cc xx xx yy s len text   text (ASCII)
//   05 cc cc xx xx yy r            dot
// Colours are RGB565, everything little-endian. The drawing area is 320 x 128.

export const AREA = { w: 320, h: 128 } as const;
export const MAX_POINTS = 80;

export type Point = { x: number; y: number };

const clampX = (x: number) => Math.max(0, Math.min(AREA.w - 1, Math.round(x)));
const clampY = (y: number) => Math.max(0, Math.min(AREA.h - 1, Math.round(y)));

// 8-bit red, green, blue → 5-6-5 bits, the screen's own format.
export const rgb565 = (r: number, g: number, b: number) =>
  ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);

const le16 = (v: number) => [v & 0xff, (v >> 8) & 0xff];

export const clear = (c: number) => Uint8Array.of(0x01, ...le16(c));

export const dot = (c: number, p: Point, r: number) =>
  Uint8Array.of(0x05, ...le16(c), ...le16(clampX(p.x)), clampY(p.y), Math.max(1, Math.min(255, r)));

export const rect = (c: number, p: Point, w: number, h: number) =>
  Uint8Array.of(
    0x03,
    ...le16(c),
    ...le16(clampX(p.x)),
    clampY(p.y),
    ...le16(Math.max(0, Math.round(w))),
    Math.max(0, Math.min(255, Math.round(h))),
  );

export function text(c: number, p: Point, size: number, s: string): Uint8Array {
  const chars = Array.from(s.slice(0, 60), (ch) => {
    const code = ch.charCodeAt(0);
    return code >= 0x20 && code < 0x7f ? code : 0x3f; // '?' for anything the screen font lacks
  });
  return Uint8Array.of(
    0x04,
    ...le16(c),
    ...le16(clampX(p.x)),
    clampY(p.y),
    Math.max(1, Math.min(4, size)),
    chars.length,
    ...chars,
  );
}

export function stroke(c: number, width: number, points: Point[]): Uint8Array {
  const pts = points.slice(0, MAX_POINTS);
  const out = [0x02, ...le16(c), Math.max(1, Math.min(255, Math.round(width))), pts.length];
  for (const p of pts) out.push(...le16(clampX(p.x)), clampY(p.y));
  return Uint8Array.from(out);
}

// How many points of a stroke fit in one write of `max` bytes.
export const pointsPerWrite = (max: number) =>
  Math.max(1, Math.min(MAX_POINTS, Math.floor((max - 5) / 3)));

// A long stroke as several writes. Each piece repeats the previous piece's last point, so the
// line on the board stays joined.
export function strokeChunks(c: number, width: number, points: Point[], max: number): Uint8Array[] {
  const per = pointsPerWrite(max);
  if (points.length <= per) return [stroke(c, width, points)];
  const out: Uint8Array[] = [];
  for (let i = 0; i < points.length - 1; i += per - 1)
    out.push(stroke(c, width, points.slice(i, i + per)));
  return out;
}
