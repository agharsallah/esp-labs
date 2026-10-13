// Pure helpers: how a card's rules read in plain words, the scenario presets, dates <-> unix
// seconds. No React, so rules.test.ts checks them directly.
import type { Card, CardPatch } from "./api.ts";

export const anyTime = (c: Pick<Card, "fromHour" | "toHour">) => c.fromHour === c.toHour;

const hh = (h: number) => `${String(h % 24).padStart(2, "0")}:00`;

// "any time" · "08:00-18:00" · "22:00-06:00 (overnight)"
export function hoursText(c: Pick<Card, "fromHour" | "toHour">): string {
  if (anyTime(c)) return "any time";
  return `${hh(c.fromHour)}-${hh(c.toHour)}${c.fromHour > c.toHour ? " (overnight)" : ""}`;
}

export function untilText(c: Pick<Card, "validUntil">, locale?: string): string {
  if (!c.validUntil) return "no end date";
  return `until ${new Date(c.validUntil * 1000).toLocaleString(locale, { dateStyle: "medium", timeStyle: "short" })}`;
}

export function usesText(c: Pick<Card, "usesLeft">): string {
  if (c.usesLeft < 0) return "unlimited";
  return c.usesLeft === 1 ? "1 use left" : `${c.usesLeft} uses left`;
}

// The card's whole rule set in one line, for the list.
export function rulesText(c: Card): string {
  const parts = [hoursText(c), untilText(c), usesText(c)].filter(
    (p) => p !== "any time" && p !== "no end date" && p !== "unlimited",
  );
  return parts.length ? parts.join(" · ") : "always";
}

// <input type="datetime-local"> value <-> unix seconds, in the browser's time zone.
export function toLocalInput(unix: number): string {
  if (!unix) return "";
  const d = new Date(unix * 1000);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`;
}
export function fromLocalInput(value: string): number {
  if (!value) return 0;
  const ms = new Date(value).getTime();
  return Number.isNaN(ms) ? 0 : Math.floor(ms / 1000);
}

// The next Sunday 20:00 after `now` (local time): the end of a weekend guest's access.
export function nextSundayEvening(now: Date): number {
  const d = new Date(now);
  d.setHours(20, 0, 0, 0);
  const days = (7 - d.getDay()) % 7;
  d.setDate(d.getDate() + days);
  if (d.getTime() <= now.getTime()) d.setDate(d.getDate() + 7);
  return Math.floor(d.getTime() / 1000);
}

export interface Preset {
  key: string;
  label: string;
  explain: string;
  patch: (now: Date) => CardPatch;
}

// Common situations, as card settings. The board enforces them; the card itself doesn't change.
export const PRESETS: Preset[] = [
  {
    key: "resident",
    label: "Resident",
    explain: "Opens any time, no end date.",
    patch: () => ({
      role: "resident",
      blocked: false,
      fromHour: 0,
      toHour: 0,
      validUntil: 0,
      usesLeft: -1,
    }),
  },
  {
    key: "office",
    label: "Office hours",
    explain: "Opens 08:00-18:00 only.",
    patch: () => ({ blocked: false, fromHour: 8, toHour: 18 }),
  },
  {
    key: "night",
    label: "Night shift",
    explain: "Opens 22:00-06:00, across midnight.",
    patch: () => ({ blocked: false, fromHour: 22, toHour: 6 }),
  },
  {
    key: "weekend",
    label: "Weekend guest",
    explain: "Guest, 10 uses, until Sunday 20:00.",
    patch: (now) => ({
      role: "guest",
      blocked: false,
      usesLeft: 10,
      validUntil: nextSundayEvening(now),
      fromHour: 0,
      toHour: 0,
    }),
  },
  {
    key: "once",
    label: "One visit",
    explain: "Guest with a single use, valid 24 h.",
    patch: (now) => ({
      role: "guest",
      blocked: false,
      usesLeft: 1,
      validUntil: Math.floor(now.getTime() / 1000) + 86_400,
      fromHour: 0,
      toHour: 0,
    }),
  },
  {
    key: "lost",
    label: "Lost card",
    explain: "Blocked at once; nothing else changes.",
    patch: () => ({ blocked: true }),
  },
];

// Another card's rules (role, hours, end date, uses), for "Copy rules from": not its name, and
// not blocked, so copying from a blocked card doesn't block this one by surprise.
export function rulesOf(c: Card): CardPatch {
  return {
    role: c.role,
    fromHour: c.fromHour,
    toHour: c.toHour,
    validUntil: c.validUntil,
    usesLeft: c.usesLeft,
  };
}

// Only the fields that differ from the card: what the PUT should send.
export function diff(card: Card, draft: CardPatch): CardPatch {
  const out: CardPatch = {};
  for (const k of Object.keys(draft) as (keyof CardPatch)[]) {
    if (draft[k] !== undefined && draft[k] !== card[k])
      (out as Record<string, unknown>)[k] = draft[k];
  }
  return out;
}

const DECISION_TONE: Record<string, "good" | "warning" | "critical"> = {
  granted: "good",
  blocked: "critical",
  "unknown card": "critical",
  "admins only": "critical",
  lockdown: "warning",
  "no clock yet": "warning",
  expired: "warning",
  "outside hours": "warning",
  "no uses left": "warning",
};
export const tone = (decision: string) => DECISION_TONE[decision] ?? "warning";
