// The Sensors notification (ba5e0011), 12 bytes, little-endian:
//   int16 temperature x 100 · uint32 touch (GPIO 1) · uint32 free heap · int8 RSSI dBm · uint8 sequence
export type Sensors = { tempC: number; touch: number; heap: number; rssi: number; seq: number };

export function decodeSensors(v: DataView): Sensors | null {
  if (v.byteLength < 12) return null;
  return {
    tempC: v.getInt16(0, true) / 100,
    touch: v.getUint32(2, true),
    heap: v.getUint32(6, true),
    rssi: v.getInt8(10),
    seq: v.getUint8(11),
  };
}

// Button (ba5e0003): gesture (1 click, 2 double-click, 3 long press), uint16 count.
export type ButtonEvent = { gesture: "click" | "double" | "long"; count: number };
export function decodeButton(v: DataView): ButtonEvent | null {
  if (v.byteLength < 3) return null;
  const g = v.getUint8(0);
  return { gesture: g === 2 ? "double" : g === 3 ? "long" : "click", count: v.getUint16(1, true) };
}

// Temperature (ba5e0004): int16 °C x 100.
export const decodeTemperature = (v: DataView) =>
  v.byteLength >= 2 ? v.getInt16(0, true) / 100 : null;
