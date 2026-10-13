import { describe, expect, it } from "vitest";
import type { Card } from "./api.ts";
import {
  diff,
  fromLocalInput,
  hoursText,
  nextSundayEvening,
  rulesOf,
  rulesText,
  toLocalInput,
  tone,
  usesText,
} from "./rules.ts";

const card: Card = {
  uid: "9C4F2A1B",
  name: "Anna",
  role: "resident",
  blocked: false,
  validUntil: 0,
  fromHour: 0,
  toHour: 0,
  usesLeft: -1,
  added: 0,
  lastSeen: 0,
  taps: 0,
  now: { front: "granted", admin: "admins only" },
};

describe("rules text", () => {
  it("reads hours, including overnight", () => {
    expect(hoursText(card)).toBe("any time");
    expect(hoursText({ fromHour: 8, toHour: 18 })).toBe("08:00-18:00");
    expect(hoursText({ fromHour: 22, toHour: 6 })).toBe("22:00-06:00 (overnight)");
  });
  it("summarises a card", () => {
    expect(rulesText(card)).toBe("always");
    expect(usesText({ usesLeft: 1 })).toBe("1 use left");
    expect(rulesText({ ...card, fromHour: 8, toHour: 18, usesLeft: 3 })).toBe(
      "08:00-18:00 · 3 uses left",
    );
  });
  it("colours decisions", () => {
    expect(tone("granted")).toBe("good");
    expect(tone("blocked")).toBe("critical");
    expect(tone("expired")).toBe("warning");
  });
});

describe("dates", () => {
  it("round-trips a datetime-local value", () => {
    const unix = fromLocalInput("2026-10-18T20:00");
    expect(toLocalInput(unix)).toBe("2026-10-18T20:00");
    expect(fromLocalInput("")).toBe(0);
  });
  it("finds the next Sunday evening", () => {
    const friday = new Date(2026, 9, 16, 12, 0); // Fri 16 Oct 2026
    expect(new Date(nextSundayEvening(friday) * 1000).getDate()).toBe(18);
    const sundayLate = new Date(2026, 9, 18, 21, 0);
    expect(new Date(nextSundayEvening(sundayLate) * 1000).getDate()).toBe(25);
  });
});

describe("diff", () => {
  it("sends only what changed", () => {
    expect(diff(card, { name: "Anna", blocked: true, usesLeft: -1 })).toEqual({ blocked: true });
  });
  it("copies another card's rules, not its name or block", () => {
    const guest = {
      ...card,
      name: "Key tag",
      role: "guest" as const,
      blocked: true,
      usesLeft: 3,
      fromHour: 8,
      toHour: 18,
    };
    expect(diff(card, rulesOf(guest))).toEqual({
      role: "guest",
      usesLeft: 3,
      fromHour: 8,
      toHour: 18,
    });
  });
});
