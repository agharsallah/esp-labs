import { describe, expect, it } from "vitest";
import type { Inspect, Snapshot } from "./api.ts";
import { compare, describe as say, fromFile, toFile, valueOf } from "./cardData.ts";

const name = `416E6E61${"0".repeat(24)}`; // "Anna", then zeros
const credits = "0A000000F5FFFFFF0A00000008F708F7"; // value 10 at block 8
const zeros = "0".repeat(32);

const snap: Snapshot = {
  id: 3,
  name: "Anna",
  uid: "9C4F2A1B",
  at: 0,
  blocks: [
    { block: 4, hex: name },
    { block: 8, hex: credits },
  ],
};

describe("blocks", () => {
  it("reads text, values and zeros", () => {
    expect(say(name)).toBe("“Anna”");
    expect(valueOf(credits)).toBe(10);
    expect(say(credits)).toBe("value 10");
    expect(say(zeros)).toBe("zeros");
  });
});

describe("compare", () => {
  it("matches a snapshot against the card on the reader", () => {
    const card: Inspect = {
      uid: "04A1B2C3",
      type: "MIFARE Classic 1K",
      listed: false,
      blocks: [
        { block: 4, hex: name },
        { block: 5, hex: zeros },
        { block: 8, hex: zeros },
        { block: 9, closed: true },
      ],
    };
    const rows = compare(snap.blocks, card);
    expect(rows.map((r) => r.match)).toEqual([
      "same",
      "not saved",
      "closed",
      "different",
      "closed",
      "closed",
    ]);
  });
});

describe("file", () => {
  it("round-trips and refuses what the board would refuse", () => {
    expect(fromFile(JSON.stringify(toFile(snap)))).toEqual({ name: "Anna", blocks: snap.blocks });
    const trailer = { ...toFile(snap), blocks: [{ block: 7, hex: zeros }] };
    expect(fromFile(JSON.stringify(trailer))).toHaveProperty("error");
    expect(fromFile("not json")).toEqual({ error: "not a JSON file" });
  });
});
