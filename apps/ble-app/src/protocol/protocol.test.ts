import { describe, expect, it } from "vitest";

import { hex } from "./bytes.ts";
import { clear, dot, rgb565, stroke, strokeChunks, text } from "./canvas.ts";
import { decode, encode, noteOn } from "./midi.ts";
import { decodeButton, decodeSensors } from "./sensors.ts";
import { decodeSpeed, startSpeed } from "./speed.ts";

describe("canvas protocol", () => {
  it("encodes the firmware header's example dot", () => {
    expect(hex(dot(rgb565(255, 0, 0), { x: 160, y: 64 }, 6))).toBe("05 00 F8 A0 00 40 06");
  });
  it("encodes clear, stroke and text like the host test", () => {
    expect(hex(clear(0))).toBe("01 00 00");
    expect(
      hex(
        stroke(0xffff, 4, [
          { x: 300, y: 16 },
          { x: 10, y: 32 },
        ]),
      ),
    ).toBe("02 FF FF 04 02 2C 01 10 0A 00 20");
    expect(hex(text(31, { x: 5, y: 7 }, 2, "hi"))).toBe("04 1F 00 05 00 07 02 02 68 69");
  });
  it("splits a long stroke into joined writes that fit", () => {
    const pts = Array.from({ length: 20 }, (_, i) => ({ x: i, y: i }));
    const chunks = strokeChunks(0, 2, pts, 20); // MTU 23 → 20 bytes → 5 points per write
    for (const c of chunks) expect(c.length).toBeLessThanOrEqual(20);
    expect(chunks.length).toBe(5); // 0-4, 4-8, 8-12, 12-16, 16-19
    expect(chunks[1]?.[5]).toBe(4); // the second piece starts where the first ended
  });
});

describe("BLE-MIDI", () => {
  it("puts the header and time stamp around a note", () => {
    // 1000 ms = 0x3E8: high 6 bits 0x07, low 7 bits 0x68.
    expect(hex(encode([[0x90, 60, 100]], 1000))).toBe("87 E8 90 3C 64");
    expect(noteOn(60, 100, 0, 1000)).toEqual(encode([[0x90, 60, 100]], 1000));
  });
  it("decodes several messages and running status", () => {
    // note on C4, then running status note on E4 (data bytes only), then a note off with its own stamp.
    const msgs = decode(Uint8Array.of(0x87, 0xe8, 0x90, 60, 100, 64, 90, 0xe9, 0x80, 60, 0));
    expect(msgs.map((m) => m.type)).toEqual(["noteOn", "noteOn", "noteOff"]);
    expect(msgs[1]).toMatchObject({ note: 64, velocity: 90 });
    expect(msgs[2]?.ts).toBe((0x07 << 7) | 0x69);
  });
  it("treats note on with velocity 0 as note off", () => {
    expect(decode(encode([[0x90, 61, 0]], 5))[0]?.type).toBe("noteOff");
  });
});

describe("notifications", () => {
  it("decodes the 12-byte sensors packet", () => {
    const b = new DataView(new ArrayBuffer(12));
    b.setInt16(0, 4123, true);
    b.setUint32(2, 31000, true);
    b.setUint32(6, 240000, true);
    b.setInt8(10, -58);
    b.setUint8(11, 7);
    expect(decodeSensors(b)).toEqual({
      tempC: 41.23,
      touch: 31000,
      heap: 240000,
      rssi: -58,
      seq: 7,
    });
    expect(decodeSensors(new DataView(new ArrayBuffer(4)))).toBeNull();
  });
  it("decodes button events", () => {
    expect(decodeButton(new DataView(Uint8Array.of(2, 5, 1).buffer))).toEqual({
      gesture: "double",
      count: 261,
    });
  });
  it("tells speed data from the summary", () => {
    expect(hex(startSpeed(50000))).toBe("01 50 C3 00 00");
    expect(decodeSpeed(new DataView(Uint8Array.of(3, 0, 9, 9).buffer))).toEqual({
      kind: "data",
      seq: 3,
      size: 4,
    });
    const s = Uint8Array.of(0xff, 0xff, 0x50, 0xc3, 0, 0, 0xe8, 0x03, 0, 0, 247, 0, 12, 0, 2);
    expect(decodeSpeed(new DataView(s.buffer))).toEqual({
      kind: "summary",
      summary: { bytes: 50000, ms: 1000, mtu: 247, intervalMs: 15, phy: 2 },
    });
  });
});
