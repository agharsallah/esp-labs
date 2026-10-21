// BLE-MIDI (the MMA/Apple spec): ordinary MIDI messages with a time stamp, in one characteristic.
// Each packet starts with a header byte 10hhhhhh (the time stamp's high 6 bits), then every
// message gets a time stamp byte 1lllllll (low 7 bits) before its status byte. A 13-bit
// millisecond clock, so it wraps every 8.192 s. Running status: a message with the same status
// may follow with just its data bytes, no time stamp and no status.

export type MidiMessage =
  | { type: "noteOn"; channel: number; note: number; velocity: number; ts: number }
  | { type: "noteOff"; channel: number; note: number; velocity: number; ts: number }
  | { type: "cc"; channel: number; controller: number; value: number; ts: number }
  | { type: "other"; status: number; data: number[]; ts: number };

export function encode(messages: number[][], ms: number): Uint8Array {
  const ts = Math.floor(ms) & 0x1fff;
  const out = [0x80 | ((ts >> 7) & 0x3f)];
  for (const m of messages) out.push(0x80 | (ts & 0x7f), ...m);
  return Uint8Array.from(out);
}

export const noteOn = (note: number, velocity = 100, channel = 0, ms = performance.now()) =>
  encode([[0x90 | channel, note & 0x7f, velocity & 0x7f]], ms);
export const noteOff = (note: number, channel = 0, ms = performance.now()) =>
  encode([[0x80 | channel, note & 0x7f, 0]], ms);

// How many data bytes follow a status byte.
function dataLength(status: number): number {
  const hi = status & 0xf0;
  if (hi === 0xc0 || hi === 0xd0) return 1;
  if (hi >= 0x80 && hi <= 0xe0) return 2;
  if (status === 0xf1 || status === 0xf3) return 1;
  if (status === 0xf2) return 2;
  return 0; // system real-time and the rest
}

export function decode(bytes: Uint8Array): MidiMessage[] {
  const out: MidiMessage[] = [];
  if (bytes.length < 3 || !((bytes[0] ?? 0) & 0x80)) return out;
  const high = (bytes[0] ?? 0) & 0x3f;
  let ts = high << 7;
  let status = 0;
  let i = 1;
  while (i < bytes.length) {
    let b = bytes[i] ?? 0;
    if (b & 0x80) {
      // A time stamp byte; a status byte may follow.
      ts = (high << 7) | (b & 0x7f);
      i++;
      b = bytes[i] ?? 0;
      if (i < bytes.length && b & 0x80) {
        status = b;
        i++;
      }
    }
    if (status === 0xf0) {
      // SysEx: skip to its end (F7, after its own time stamp byte).
      while (i < bytes.length && bytes[i] !== 0xf7) i++;
      i++;
      status = 0;
      continue;
    }
    if (!status) break;
    const n = dataLength(status);
    if (i + n > bytes.length) break;
    const data = Array.from(bytes.slice(i, i + n));
    i += n;
    const channel = status & 0x0f;
    const kind = status & 0xf0;
    const [d1 = 0, d2 = 0] = data;
    if (kind === 0x90 && d2 > 0) out.push({ type: "noteOn", channel, note: d1, velocity: d2, ts });
    else if (kind === 0x80 || (kind === 0x90 && d2 === 0))
      out.push({ type: "noteOff", channel, note: d1, velocity: d2, ts });
    else if (kind === 0xb0) out.push({ type: "cc", channel, controller: d1, value: d2, ts });
    else out.push({ type: "other", status, data, ts });
    if (n === 0) status = 0;
  }
  return out;
}

const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
export const noteName = (n: number) => `${NAMES[n % 12]}${Math.floor(n / 12) - 1}`;
export const frequency = (n: number) => 440 * 2 ** ((n - 69) / 12);
