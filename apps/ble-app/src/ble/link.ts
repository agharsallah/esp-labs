// The one connection to the board, and everything that crosses it.
//
// Web Bluetooth gives the page a GATT client: requestDevice() asks the person to pick a device
// (the browser shows the chooser; a page can never connect silently), then we discover the
// services and keep handles to the characteristics the tabs use. Every read, write and
// notification goes through here, so the Connect tab can show them byte by byte.
//
// GATT allows one operation at a time per connection; Chrome throws "GATT operation already in
// progress" otherwise. So every operation waits its turn in one queue.
import { hex, viewBytes } from "../protocol/bytes.ts";
import { BENCH_SERVICE, CHR, type ChrKey, MIDI_SERVICE, nameOf, shortUuid } from "./uuids.ts";

export type Dir = "tx" | "rx" | "info" | "error";
export type LogEntry = {
  id: number;
  at: number;
  dir: Dir;
  op: string;
  what: string;
  bytes: string;
};

export type ExplorerChr = { uuid: string; name: string | null; props: string[] };
export type ExplorerService = { uuid: string; name: string | null; chars: ExplorerChr[] };

export type Status = "idle" | "choosing" | "connecting" | "connected" | "lost";

type Listener = () => void;
type NotifyHandler = (v: DataView) => void;

export const webBluetooth = () =>
  typeof navigator !== "undefined" && !!navigator.bluetooth && window.isSecureContext;

const PROPS: [keyof BluetoothCharacteristicProperties, string][] = [
  ["read", "read"],
  ["write", "write"],
  ["writeWithoutResponse", "write no rsp"],
  ["notify", "notify"],
  ["indicate", "indicate"],
];

class Link {
  status: Status = "idle";
  deviceName = "";
  error = "";
  services: ExplorerService[] = [];
  log: LogEntry[] = [];
  /** The largest write that has worked; starts optimistic, falls back to 20 (MTU 23) on failure. */
  maxWrite = 240;
  connectedAt = 0;

  private device: BluetoothDevice | null = null;
  private chars = new Map<ChrKey, BluetoothRemoteGATTCharacteristic>();
  private handlers = new Map<ChrKey, Set<NotifyHandler>>();
  private listeners = new Set<Listener>();
  private queue: Promise<unknown> = Promise.resolve();
  private pending = 0;
  private nextId = 1;
  private version = 0;

  // ---- for React (useSyncExternalStore) -----------------------------------------------------
  subscribe = (l: Listener) => {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  };
  snapshot = () => this.version;
  private changed() {
    this.version++;
    for (const l of this.listeners) l();
  }

  note(dir: Dir, op: string, what: string, bytes: Uint8Array | null = null) {
    this.log = [
      ...this.log.slice(-199),
      { id: this.nextId++, at: performance.now(), dir, op, what, bytes: bytes ? hex(bytes) : "" },
    ];
    this.changed();
  }

  has(key: ChrKey) {
    return this.chars.has(key);
  }

  // ---- connect --------------------------------------------------------------------------------

  async connect() {
    if (!webBluetooth()) return;
    this.error = "";
    this.status = "choosing";
    this.changed();
    try {
      // Any bench lab (its service or its name), or any BLE-MIDI instrument (B7).
      const device = await navigator.bluetooth!.requestDevice({
        filters: [
          { services: [BENCH_SERVICE] },
          { namePrefix: "Bench" },
          { services: [MIDI_SERVICE] },
        ],
        optionalServices: [BENCH_SERVICE, 0x180a, 0x180f, MIDI_SERVICE],
      });
      this.device = device;
      this.deviceName = device.name ?? "unnamed";
      device.addEventListener("gattserverdisconnected", this.onLost);
      this.status = "connecting";
      this.changed();
      this.note("info", "chooser", `you picked ${this.deviceName}`);
      const server = await device.gatt!.connect();
      this.note("info", "connect", "GATT connected: discovering services");
      await this.discover(server);
      this.status = "connected";
      this.connectedAt = performance.now();
      this.changed();
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e);
      // Closing the chooser is not an error worth shouting about.
      this.status = this.device?.gatt?.connected ? "connected" : "idle";
      if (!/cancel/i.test(msg)) {
        this.error = msg;
        this.note("error", "connect", msg);
      }
      this.changed();
    }
  }

  disconnect() {
    this.device?.gatt?.disconnect();
  }

  private onLost = () => {
    this.chars.clear();
    this.status = "lost";
    this.note("error", "link", "connection lost (out of range, board reset, or disconnected)");
  };

  private async discover(server: BluetoothRemoteGATTServer) {
    this.chars.clear();
    const services = await server.getPrimaryServices();
    const out: ExplorerService[] = [];
    for (const s of services) {
      const entry: ExplorerService = { uuid: s.uuid, name: nameOf(s.uuid), chars: [] };
      let chars: BluetoothRemoteGATTCharacteristic[] = [];
      try {
        chars = await s.getCharacteristics();
      } catch {
        // Some services are hidden from web pages (the browser's blocklist).
      }
      for (const c of chars) {
        entry.chars.push({
          uuid: c.uuid,
          name: nameOf(c.uuid),
          props: PROPS.filter(([k]) => c.properties[k]).map(([, n]) => n),
        });
        for (const [key, def] of Object.entries(CHR) as [ChrKey, (typeof CHR)[ChrKey]][])
          if (def.uuid === c.uuid) {
            this.chars.set(key, c);
            c.addEventListener("characteristicvaluechanged", () => this.onNotify(key, c));
          }
      }
      out.push(entry);
    }
    this.services = out;
    this.note(
      "info",
      "discover",
      `${out.length} services, ${out.reduce((n, s) => n + s.chars.length, 0)} characteristics`,
    );
  }

  // ---- operations -------------------------------------------------------------------------------

  /** Runs GATT operations one at a time, in order. */
  private enqueue<T>(op: () => Promise<T>): Promise<T> {
    this.pending++;
    const run = this.queue.then(op, op);
    this.queue = run.catch(() => undefined).finally(() => this.pending--);
    return run;
  }

  /** How many operations are waiting: the sketch skips frames when the radio is behind. */
  get backlog() {
    return this.pending;
  }

  async read(key: ChrKey): Promise<DataView | null> {
    const c = this.chars.get(key);
    if (!c) return null;
    try {
      const v = await this.enqueue(() => c.readValue());
      this.note("rx", "read", CHR[key].name, viewBytes(v));
      return v;
    } catch (e) {
      this.note("error", "read", `${CHR[key].name}: ${(e as Error).message}`);
      return null;
    }
  }

  /** withResponse: the board confirms each write (slower, but errors come back). */
  async write(
    key: ChrKey,
    bytes: Uint8Array,
    withResponse = true,
    quiet = false,
  ): Promise<boolean> {
    const c = this.chars.get(key);
    if (!c) return false;
    try {
      await this.enqueue(() =>
        withResponse ? c.writeValueWithResponse(bytes) : c.writeValueWithoutResponse(bytes),
      );
      if (!quiet) this.note("tx", withResponse ? "write" : "write cmd", CHR[key].name, bytes);
      return true;
    } catch (e) {
      const msg = (e as Error).message;
      // A write longer than the link's MTU fails: remember the small size and let the caller retry.
      if (bytes.length > 20 && this.maxWrite > 20) {
        this.maxWrite = 20;
        this.note(
          "info",
          "mtu",
          `${bytes.length}-byte write refused: falling back to 20 bytes per write (MTU 23)`,
        );
      } else {
        this.note("error", "write", `${CHR[key].name}: ${msg}`);
      }
      return false;
    }
  }

  /** Subscribe: the browser writes 01 00 into the characteristic's CCCD (2902) for us. */
  async listen(key: ChrKey, handler: NotifyHandler): Promise<() => void> {
    let set = this.handlers.get(key);
    if (!set) this.handlers.set(key, (set = new Set()));
    set.add(handler);
    const c = this.chars.get(key);
    if (c && set.size === 1) {
      try {
        await this.enqueue(() => c.startNotifications());
        this.note("tx", "subscribe", `${CHR[key].name} (CCCD = 01 00)`);
      } catch (e) {
        this.note("error", "subscribe", `${CHR[key].name}: ${(e as Error).message}`);
      }
    }
    return () => {
      set.delete(handler);
      if (set.size === 0 && c && this.status === "connected")
        this.enqueue(() => c.stopNotifications())
          .then(() => this.note("tx", "unsubscribe", `${CHR[key].name} (CCCD = 00 00)`))
          .catch(() => undefined);
    };
  }

  private onNotify(key: ChrKey, c: BluetoothRemoteGATTCharacteristic) {
    const v = c.value;
    if (!v) return;
    // The speed test sends hundreds per second: log those as a count, not one line each.
    if (key !== "speed" && key !== "sensors")
      this.note("rx", "notify", CHR[key].name, viewBytes(v));
    for (const h of this.handlers.get(key) ?? []) h(v);
  }

  explorerName(uuid: string) {
    return nameOf(uuid) ?? shortUuid(uuid);
  }
}

export const link = new Link();
