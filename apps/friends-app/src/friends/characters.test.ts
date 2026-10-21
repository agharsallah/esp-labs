import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { FRIENDS, friendById } from "./characters.ts";

// The firmware draws the same friends on the board's screen: ids and names must match.
const firmware = readFileSync(
  new URL("../../../../labs/rfid-lab-r8/src/characters.h", import.meta.url),
  "utf8",
);
const onBoard = [...firmware.matchAll(/\{"(\w+)", "(\w+)", Kind::/g)].map((m) => [m[1], m[2]]);

describe("the cast", () => {
  it("has the same friends, in the same order, as the firmware", () => {
    expect(onBoard.length).toBeGreaterThan(0);
    expect(FRIENDS.map((f) => [f.id, f.name])).toEqual(onBoard);
  });

  it("gives every friend its own move, sparkles and sky", () => {
    for (const key of ["move", "effect"] as const)
      expect(new Set(FRIENDS.map((f) => f[key])).size).toBe(FRIENDS.length);
    expect(new Set(FRIENDS.map((f) => f.colors.skyTop)).size).toBe(FRIENDS.length);
  });

  it("knows nobody for an empty card", () => {
    expect(friendById("")).toBeUndefined();
    expect(friendById("fox")?.name).toBe("Foxy");
  });
});
