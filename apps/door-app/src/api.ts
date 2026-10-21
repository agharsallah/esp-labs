// The board's API (labs/rfid-lab-r7/src/door_api.h), typed and checked with zod: if the firmware and
// the app ever disagree, the app says so instead of showing nonsense.
import { z } from "zod";

export const Role = z.enum(["resident", "guest", "maintenance", "admin"]);
export type Role = z.infer<typeof Role>;

// Two simulated locks; the one reader is mounted at one of them at a time.
export const LockId = z.enum(["front", "admin"]);
export type LockId = z.infer<typeof LockId>;

export const Card = z.object({
  uid: z.string(),
  name: z.string(),
  role: Role,
  blocked: z.boolean(),
  validUntil: z.number(), // unix seconds, 0 = no end date
  fromHour: z.number(),
  toHour: z.number(), // equal to fromHour = any time
  usesLeft: z.number(), // -1 = unlimited
  added: z.number(),
  lastSeen: z.number(),
  taps: z.number(),
  now: z.object({ front: z.string(), admin: z.string() }), // what a tap would do right now at each lock
});
export type Card = z.infer<typeof Card>;

export const Status = z.object({
  chip: z.object({ version: z.string(), name: z.string(), ok: z.boolean() }),
  wifi: z.object({ ssid: z.string(), ip: z.string(), rssi: z.number() }),
  clock: z.object({ synced: z.boolean(), now: z.number() }),
  locks: z.array(
    z.object({
      id: LockId,
      title: z.string(),
      mode: z.enum(["locked", "open", "denied"]),
      msLeft: z.number(),
      name: z.string(),
      uid: z.string(),
      why: z.string(),
    }),
  ),
  readerAt: LockId,
  present: z.string().nullable(),
  // Waiting for a card to be laid down: a new one, a replacement, or copy's second card.
  waiting: z
    .object({
      for: z.enum(["enroll", "replace", "copy"]),
      name: z.string(), // enroll, replace: the card's name; copy: where the data comes from
      uid: z.string(), // replace: the card being replaced; copy: the source card
      msLeft: z.number(),
    })
    .nullable(),
  lockdown: z.boolean(),
  cards: z.number(),
  busy: z.boolean(),
  // Newer firmware only (optional, so an older rfid-lab-r7 still connects).
  reader: z
    .object({
      wait: z.enum(["irq", "poll"]), // IRQ wire, or polling the chip
      restores: z.number(), // the chip reset itself (3.3 V dip) and got its settings back
      lost: z.number(), // the chip stopped answering on SPI
      problem: z.string(), // the last thing that went wrong, "" if nothing
      problemAgo: z.number(), // seconds
    })
    .optional(),
  board: z.object({ uptime: z.number(), reset: z.string(), heap: z.number() }).optional(),
});
export type Status = z.infer<typeof Status>;

export const DoorEvent = z.object({
  seq: z.number(),
  at: z.number(),
  uid: z.string(),
  name: z.string(),
  what: z.string(),
  decision: z.string(),
  lock: z.string().optional(), // taps and remote opens: "front" or "admin"
});
export type DoorEvent = z.infer<typeof DoorEvent>;

export const JobResult = z.object({
  id: z.number(),
  ok: z.boolean(),
  message: z.string(),
  data: z.unknown().optional(),
});
export type JobResult = z.infer<typeof JobResult>;

export const InspectBlock = z.object({
  block: z.number(),
  closed: z.boolean().optional(),
  hex: z.string().optional(),
  text: z.string().optional(),
  value: z.number().optional(),
});
export const Inspect = z.object({
  uid: z.string(),
  type: z.string(),
  listed: z.boolean(),
  name: z.string().optional(),
  blocks: z.array(InspectBlock),
});
export type Inspect = z.infer<typeof Inspect>;

// Saved card data: the lab blocks (4-6, 8-10) that were readable, 16 bytes each as 32 hex digits.
export const SavedBlock = z.object({ block: z.number(), hex: z.string() });
export type SavedBlock = z.infer<typeof SavedBlock>;
export const Snapshot = z.object({
  id: z.number(),
  name: z.string(),
  uid: z.string(), // "" when imported from a file
  at: z.number(),
  blocks: z.array(SavedBlock),
});
export type Snapshot = z.infer<typeof Snapshot>;

export const Message = z.discriminatedUnion("type", [
  z.object({ type: z.literal("status"), data: Status }),
  z.object({ type: z.literal("cards"), data: z.array(Card) }),
  z.object({ type: z.literal("snapshots"), data: z.array(Snapshot) }),
  z.object({ type: z.literal("event"), data: DoorEvent }),
  z.object({ type: z.literal("job"), data: JobResult }),
]);
export type Message = z.infer<typeof Message>;

export type CardPatch = Partial<
  Pick<Card, "name" | "role" | "blocked" | "validUntil" | "fromHour" | "toHour" | "usesLeft">
>;

export class ApiError extends Error {
  readonly status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}

async function call(method: string, path: string, body?: unknown): Promise<unknown> {
  let res: Response;
  try {
    res = await fetch(path, {
      method,
      headers: body === undefined ? undefined : { "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new ApiError(
      0,
      "The board isn't reachable. Is it on your Wi-Fi with rfid-lab-r7, and is BENCH_URL right?",
    );
  }
  const text = await res.text();
  const json: unknown = text ? JSON.parse(text) : {};
  if (!res.ok) {
    const message =
      typeof json === "object" && json !== null && "error" in json
        ? String(json.error)
        : `HTTP ${res.status}`;
    throw new ApiError(
      res.status,
      res.status === 401
        ? "The admin token is missing or wrong: set BENCH_TOKEN in door-app/.env (serial: token), then restart pnpm dev."
        : message,
    );
  }
  return json;
}

const Job = z.object({ job: z.number() });
const q = (uid: string) => `?uid=${encodeURIComponent(uid)}`;

export const api = {
  cards: async () => z.array(Card).parse(await call("GET", "/api/cards")),
  events: async (since = 0) =>
    z.array(DoorEvent).parse(await call("GET", `/api/events?since=${since}`)),
  enroll: async (name: string, role: Role) =>
    Job.parse(await call("POST", "/api/enroll", { name, role })).job,
  replace: async (uid: string) => Job.parse(await call("POST", `/api/replace${q(uid)}`)).job,
  cancel: async () => Job.parse(await call("POST", "/api/cancel")).job,
  update: async (uid: string, patch: CardPatch) =>
    z.object({ card: Card }).parse(await call("PUT", `/api/card${q(uid)}`, patch)).card,
  remove: async (uid: string) => void (await call("DELETE", `/api/card${q(uid)}`)),
  label: async (uid: string) => Job.parse(await call("POST", `/api/label${q(uid)}`)).job,
  wipe: async (uid: string) => Job.parse(await call("POST", `/api/wipe${q(uid)}`)).job,
  inspect: async () => Job.parse(await call("POST", "/api/inspect")).job,
  snapshots: async () => z.array(Snapshot).parse(await call("GET", "/api/snapshots")),
  save: async (name: string) => Job.parse(await call("POST", "/api/save", { name })).job,
  importSnapshot: async (name: string, blocks: SavedBlock[]) =>
    Snapshot.parse(await call("POST", "/api/import", { name, blocks })),
  restore: async (id: number) => Job.parse(await call("POST", `/api/restore?id=${id}`)).job,
  removeSnapshot: async (id: number) => void (await call("DELETE", `/api/snapshot?id=${id}`)),
  copy: async () => Job.parse(await call("POST", "/api/copy")).job,
  open: async (lock: LockId) => Job.parse(await call("POST", `/api/open?lock=${lock}`)).job,
  readerAt: async (lock: LockId) => void (await call("PUT", "/api/reader-at", { lock })),
  lockdown: async (on: boolean) => void (await call("PUT", "/api/lockdown", { on })),
};
