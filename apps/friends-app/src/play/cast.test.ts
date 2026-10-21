import { describe, expect, it } from "vitest";
import type { Show } from "../board/types.ts";
import { follow, MAX_ON_STAGE, onShow, prune, STAY_MS } from "./cast.ts";

const show = (uid: string, character: string, extra: Partial<Show> = {}): Show => ({
  seq: 1,
  uid,
  character,
  name: "",
  number: 1,
  taps: 1,
  isNew: false,
  full: false,
  fromApp: false,
  ...extra,
});

describe("the stage cast", () => {
  it("puts card 1's and card 2's different friends on stage together, newest first", () => {
    const cast = onShow(onShow([], show("A", "fox"), 0), show("B", "robot"), 10);
    expect(cast.map((a) => a.character)).toEqual(["robot", "fox"]);
  });

  it("plays the move again when the same card comes back, without a second actor", () => {
    const once = onShow([], show("A", "fox"), 0);
    const twice = onShow(once, show("A", "fox"), 5);
    expect(twice).toHaveLength(1);
    expect(twice[0]?.cue).toBe(2);
  });

  it("leaves empty cards and a full list off the stage", () => {
    expect(onShow([], show("A", ""), 0)).toEqual([]);
    expect(onShow([], show("A", "fox", { full: true }), 0)).toEqual([]);
  });

  it(`keeps at most ${MAX_ON_STAGE} friends and lets old ones go, never the newest`, () => {
    let cast = ["A", "B", "C", "D"].reduce(
      (c, uid, i) => onShow(c, show(uid, "cat"), i),
      [] as ReturnType<typeof onShow>,
    );
    expect(cast.map((a) => a.uid)).toEqual(["D", "C", "B"]);
    cast = prune(cast, 3 + STAY_MS);
    expect(cast.map((a) => a.uid)).toEqual(["D"]);
  });

  it("follows a friend dragged onto a card, and drops one whose card was emptied", () => {
    const cast = onShow(onShow([], show("A", "fox"), 0), show("B", "frog"), 1);
    const next = follow(cast, [
      { uid: "A", number: 1, character: "dragon", name: "Spark", taps: 2 },
      { uid: "B", number: 2, character: "", name: "", taps: 1 },
    ]);
    expect(next.map((a) => [a.uid, a.character, a.name])).toEqual([["A", "dragon", "Spark"]]);
  });
});
