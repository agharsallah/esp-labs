// Pure helpers for saved card data: what a block says, comparing a snapshot with the card on the
// reader, and the JSON file the app downloads and imports. No React, so cardData.test.ts checks
// them directly.
import { z } from "zod";
import type { Inspect, SavedBlock, Snapshot } from "./api.ts";

// The lab blocks: sector 1 (4, 5, 6) and sector 2 (8, 9, 10). Never block 0 (the UID) or a
// sector trailer (keys and access bits): the board refuses those.
export const LAB_BLOCKS = [4, 5, 6, 8, 9, 10] as const;
export const SECTORS = [
  { sector: 1, blocks: [4, 5, 6] },
  { sector: 2, blocks: [8, 9, 10] },
] as const;

const HEX32 = /^[0-9A-F]{32}$/i;

const bytes = (hex: string) =>
  Array.from({ length: hex.length / 2 }, (_, i) => parseInt(hex.slice(i * 2, i * 2 + 2), 16));

export const isBlank = (hex: string) => /^0+$/.test(hex);

// Printable ASCII as text, everything else as a dot (like the board's inspect).
export function blockText(hex: string): string {
  return bytes(hex)
    .map((b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : "."))
    .join("");
}

// A MIFARE value block (R5's credits): value, ~value, value, then the address 4 times
// (addr, ~addr, addr, ~addr). Returns the value, or undefined when the block isn't one.
export function valueOf(hex: string): number | undefined {
  if (!HEX32.test(hex)) return undefined;
  const all = bytes(hex);
  const b = (i: number) => all[i] ?? 0;
  const word = (i: number) => (b(i) | (b(i + 1) << 8) | (b(i + 2) << 16) | (b(i + 3) << 24)) >>> 0;
  const v = word(0);
  const ok =
    word(4) === ~v >>> 0 &&
    word(8) === v &&
    b(12) === b(14) &&
    b(13) === b(15) &&
    b(12) === (~b(13) & 0xff);
  return ok ? v | 0 : undefined;
}

// One line per block for people: a value, the text, or "zeros".
export function describe(hex: string): string {
  if (isBlank(hex)) return "zeros";
  const v = valueOf(hex);
  if (v !== undefined) return `value ${v}`;
  return `“${blockText(hex).replace(/\.+$/, "")}”`;
}

export type Match = "same" | "different" | "not saved" | "closed";
export interface CompareRow {
  block: number;
  saved?: string;
  card?: string;
  match: Match;
}

// The snapshot next to what the card on the reader holds now, block by block.
export function compare(saved: SavedBlock[], card: Inspect): CompareRow[] {
  return LAB_BLOCKS.map((block) => {
    const s = saved.find((b) => b.block === block)?.hex;
    const c = card.blocks.find((b) => b.block === block);
    if (!c || c.closed || !c.hex) return { block, saved: s, match: "closed" };
    if (!s) return { block, card: c.hex, match: "not saved" };
    return {
      block,
      saved: s,
      card: c.hex,
      match: s.toUpperCase() === c.hex.toUpperCase() ? "same" : "different",
    };
  });
}

// ---- The file -------------------------------------------------------------------------------

const KIND = "bench-door/card-data";

export function toFile(s: Snapshot) {
  return { kind: KIND, version: 1, name: s.name, uid: s.uid, at: s.at, blocks: s.blocks };
}

export const fileName = (s: Snapshot) =>
  `card-data-${s.name.replace(/[^A-Za-z0-9_-]+/g, "-").toLowerCase() || s.id}.json`;

const FileSchema = z.object({
  kind: z.literal(KIND),
  version: z.literal(1),
  name: z.string(),
  uid: z.string().optional(),
  at: z.number().optional(),
  blocks: z
    .array(
      z.object({
        block: z
          .number()
          .refine((n) => (LAB_BLOCKS as readonly number[]).includes(n), "a lab block (4-6, 8-10)"),
        hex: z.string().regex(HEX32, "32 hex digits"),
      }),
    )
    .min(1)
    .max(6)
    .refine((list) => new Set(list.map((b) => b.block)).size === list.length, "each block once"),
});

// The board only takes plain names (A-Z, digits, space, - . ' _), 1-23 characters.
export const cleanName = (name: string) =>
  name
    .replace(/[^A-Za-z0-9 .'_-]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 23) || "Imported";

export function fromFile(text: string): { name: string; blocks: SavedBlock[] } | { error: string } {
  let json: unknown;
  try {
    json = JSON.parse(text);
  } catch {
    return { error: "not a JSON file" };
  }
  const parsed = FileSchema.safeParse(json);
  if (!parsed.success) {
    const issue = parsed.error.issues[0];
    const where = issue?.path.join(".") || "file";
    return {
      error: `not a card-data file from this app (${where}: ${issue?.message ?? "unexpected shape"})`,
    };
  }
  return {
    name: cleanName(parsed.data.name),
    blocks: parsed.data.blocks.map((b) => ({ block: b.block, hex: b.hex.toUpperCase() })),
  };
}
