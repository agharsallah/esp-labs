// The speed test (ba5e0012). Write 01 + uint32 bytes to start, 00 to stop. The board notifies
// packets of MTU - 3 bytes that start with a uint16 sequence number, then one summary:
//   FF FF · uint32 bytes · uint32 ms · uint16 MTU · uint16 interval (1.25 ms units) · uint8 PHY
export const startSpeed = (bytes: number) => {
  const b = new Uint8Array(5);
  b[0] = 0x01;
  new DataView(b.buffer).setUint32(1, bytes, true);
  return b;
};
export const stopSpeed = () => Uint8Array.of(0x00);

export type SpeedSummary = {
  bytes: number;
  ms: number;
  mtu: number;
  intervalMs: number;
  phy: number;
};

export type SpeedPacket =
  { kind: "data"; seq: number; size: number } | { kind: "summary"; summary: SpeedSummary };

export function decodeSpeed(v: DataView): SpeedPacket | null {
  if (v.byteLength < 2) return null;
  const seq = v.getUint16(0, true);
  if (seq === 0xffff && v.byteLength >= 15)
    return {
      kind: "summary",
      summary: {
        bytes: v.getUint32(2, true),
        ms: v.getUint32(6, true),
        mtu: v.getUint16(10, true),
        intervalMs: v.getUint16(12, true) * 1.25,
        phy: v.getUint8(14),
      },
    };
  return { kind: "data", seq, size: v.byteLength };
}

// Link (ba5e0013): one byte.
export const LINK_OPS = { fast: 1, slow: 2, phy2m: 3, phy1m: 4, dle: 5 } as const;
