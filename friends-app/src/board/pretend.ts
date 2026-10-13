// A pretend board in the browser: plays by the firmware's rules (friends.h, tap_reader.h) so the
// app can be tried, shown and tested without the ESP32. New cards add themselves on their first
// tap, up to 12; a show from the app doesn't count as a visit.
import type { BoardLink, CardPatch, FriendCard, Message, Show, Status } from "./types.ts";
import { BoardError } from "./types.ts";

export const MAX_CARDS = 12;

/** Two cards to start with, like the white card and the key tag on the bench. */
const STARTER: FriendCard[] = [
  { uid: "9C4F2A1B", number: 1, character: "fox", name: "", taps: 3 },
  { uid: "04A32B91", number: 2, character: "", name: "", taps: 1 },
];

export class PretendBoard implements BoardLink {
  readonly kind = "pretend" as const;
  private cards: FriendCard[];
  private next: number;
  private seq = 0;
  private present: string | null = null;
  private listeners = new Set<(m: Message) => void>();
  private readonly started = Date.now();

  constructor(cards: FriendCard[] = STARTER) {
    this.cards = cards.map((c) => ({ ...c }));
    this.next = Math.max(0, ...this.cards.map((c) => c.number)) + 1;
  }

  subscribe(onMessage: (m: Message) => void, onConnected: (up: boolean) => void) {
    this.listeners.add(onMessage);
    onConnected(true);
    onMessage({ type: "status", data: this.status() });
    onMessage({ type: "cards", data: this.list() });
    const timer = setInterval(() => onMessage({ type: "status", data: this.status() }), 1000);
    return () => {
      clearInterval(timer);
      this.listeners.delete(onMessage);
    };
  }

  list(): FriendCard[] {
    return this.cards.map((c) => ({ ...c }));
  }

  /** A card laid on the pretend reader: counted, or added when new. */
  tap(uid: string): Show {
    let card = this.cards.find((c) => c.uid === uid);
    let isNew = false;
    let full = false;
    if (!card) {
      if (this.cards.length >= MAX_CARDS) full = true;
      else {
        card = { uid, number: this.next++, character: "", name: "", taps: 0 };
        this.cards.push(card);
        isNew = true;
      }
    }
    if (card) card.taps++;
    const show = this.announce(card ?? { uid, number: 0, character: "", name: "", taps: 0 }, {
      isNew,
      full,
      fromApp: false,
    });
    this.present = uid;
    setTimeout(() => {
      if (this.present === uid) this.present = null;
    }, 1500);
    if (!full) this.emit({ type: "cards", data: this.list() });
    return show;
  }

  /** A card the pretend reader has never seen, with a made-up UID. */
  newUid(): string {
    const bytes = crypto.getRandomValues(new Uint8Array(4));
    return Array.from(bytes, (b) => b.toString(16).padStart(2, "0").toUpperCase()).join("");
  }

  async update(uid: string, patch: CardPatch): Promise<FriendCard> {
    const card = this.cards.find((c) => c.uid === uid);
    if (!card) throw new BoardError(404, "no card with this uid in the list");
    if (patch.name !== undefined && patch.name.length > 15)
      throw new BoardError(400, "name: up to 15 plain characters");
    if (patch.character !== undefined) card.character = patch.character;
    if (patch.name !== undefined) card.name = patch.name;
    this.emit({ type: "cards", data: this.list() });
    return { ...card };
  }

  async forget(uid: string): Promise<void> {
    const before = this.cards.length;
    this.cards = this.cards.filter((c) => c.uid !== uid);
    if (this.cards.length === before)
      throw new BoardError(404, "no card with this uid in the list");
    if (this.cards.length === 0) this.next = 1;
    this.emit({ type: "cards", data: this.list() });
  }

  async show(uid: string): Promise<void> {
    const card = this.cards.find((c) => c.uid === uid);
    if (!card) throw new BoardError(404, "no card with this uid in the list");
    this.announce(card, { isNew: false, full: false, fromApp: true });
  }

  async chipReset(): Promise<void> {}

  private announce(card: FriendCard, how: Pick<Show, "isNew" | "full" | "fromApp">): Show {
    const show: Show = {
      seq: ++this.seq,
      uid: card.uid,
      character: card.character,
      name: card.name,
      number: card.number,
      taps: card.taps,
      ...how,
    };
    this.emit({ type: "tap", data: show });
    return show;
  }

  private emit(m: Message) {
    for (const l of this.listeners) l(m);
  }

  private status(): Status {
    return {
      chip: { version: "B2", name: "pretend reader", ok: true },
      wifi: { ssid: "", ip: "", rssi: 0 },
      reader: { wait: "poll", restores: 0, lost: 0, problem: "", problemAgo: 0 },
      present: this.present,
      cards: this.cards.length,
      maxCards: MAX_CARDS,
      view: "stage",
      board: { uptime: Math.round((Date.now() - this.started) / 1000), reset: "pretend", heap: 0 },
    };
  }
}
