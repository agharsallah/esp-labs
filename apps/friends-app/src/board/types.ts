// The board's API (labs/rfid-lab-r8/src/friends_api.h), typed and checked with zod: if the firmware
// and the app ever disagree, the app says so instead of showing nonsense.
import { z } from "zod";

export const FriendCard = z.object({
  uid: z.string(),
  number: z.number(), // 1, 2, 3 ... in the order the cards were first tapped
  character: z.string(), // a cast id, "" = nobody lives here yet
  name: z.string(), // the friend's name on this card, "" = the character's own name
  taps: z.number(),
});
export type FriendCard = z.infer<typeof FriendCard>;

export const Status = z.object({
  chip: z.object({ version: z.string(), name: z.string(), ok: z.boolean() }),
  wifi: z.object({ ssid: z.string(), ip: z.string(), rssi: z.number() }),
  reader: z.object({
    wait: z.enum(["irq", "poll"]),
    restores: z.number(),
    lost: z.number(),
    problem: z.string(),
    problemAgo: z.number(),
  }),
  present: z.string().nullable(), // the UID of the card on the reader
  cards: z.number(),
  maxCards: z.number(),
  view: z.string(),
  board: z.object({ uptime: z.number(), reset: z.string(), heap: z.number() }),
});
export type Status = z.infer<typeof Status>;

/** A show: a card was tapped on the reader, or the app asked the board to show it. */
export const Show = z.object({
  seq: z.number(),
  uid: z.string(),
  character: z.string(),
  name: z.string(),
  number: z.number(),
  taps: z.number(),
  isNew: z.boolean(), // its first tap: the board just added it
  full: z.boolean(), // a new card, but the list is full
  fromApp: z.boolean(),
});
export type Show = z.infer<typeof Show>;

export const Message = z.discriminatedUnion("type", [
  z.object({ type: z.literal("status"), data: Status }),
  z.object({ type: z.literal("cards"), data: z.array(FriendCard) }),
  z.object({ type: z.literal("tap"), data: Show }),
]);
export type Message = z.infer<typeof Message>;

export interface CardPatch {
  character?: string;
  name?: string;
}

/** Everything the app can ask of a board, real or pretend (live.ts, pretend.ts). */
export interface BoardLink {
  readonly kind: "live" | "pretend";
  /** Messages as they arrive, and whether the link is up. Returns an unsubscribe. */
  subscribe(onMessage: (m: Message) => void, onConnected: (up: boolean) => void): () => void;
  update(uid: string, patch: CardPatch): Promise<FriendCard>;
  forget(uid: string): Promise<void>;
  show(uid: string): Promise<void>;
  chipReset(): Promise<void>;
}

export class BoardError extends Error {
  readonly status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}
