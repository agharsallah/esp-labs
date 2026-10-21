// Every UUID the bench labs use. The Bench service is ours (B3); 180A, 180F and BLE-MIDI are standard.
export const benchUuid = (n: number) =>
  `ba5e${n.toString(16).padStart(4, "0")}-b1e5-4ab1-9e5c-be7cbe7cb0a7`;

export const BENCH_SERVICE = benchUuid(0);
export const MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700";
export const MIDI_IO = "7772e5db-3868-4112-a1a9-f2669d106bf3";

// The characteristics the tabs use, by a short key.
export const CHR = {
  led: { service: BENCH_SERVICE, uuid: benchUuid(0x01), name: "LED" },
  message: { service: BENCH_SERVICE, uuid: benchUuid(0x02), name: "Message" },
  button: { service: BENCH_SERVICE, uuid: benchUuid(0x03), name: "Button" },
  temperature: { service: BENCH_SERVICE, uuid: benchUuid(0x04), name: "Temperature" },
  uptime: { service: BENCH_SERVICE, uuid: benchUuid(0x05), name: "Uptime" },
  vault: { service: BENCH_SERVICE, uuid: benchUuid(0x06), name: "Vault" },
  canvas: { service: BENCH_SERVICE, uuid: benchUuid(0x10), name: "Canvas" },
  sensors: { service: BENCH_SERVICE, uuid: benchUuid(0x11), name: "Sensors" },
  speed: { service: BENCH_SERVICE, uuid: benchUuid(0x12), name: "Speed" },
  link: { service: BENCH_SERVICE, uuid: benchUuid(0x13), name: "Link" },
  midi: { service: MIDI_SERVICE, uuid: MIDI_IO, name: "MIDI I/O" },
} as const;

export type ChrKey = keyof typeof CHR;

// Friendly names for the explorer; anything else shows its UUID.
const NAMES: Record<string, string> = {
  "00001800-0000-1000-8000-00805f9b34fb": "Generic Access",
  "00001801-0000-1000-8000-00805f9b34fb": "Generic Attribute",
  "0000180a-0000-1000-8000-00805f9b34fb": "Device Information",
  "0000180f-0000-1000-8000-00805f9b34fb": "Battery",
  "00002a19-0000-1000-8000-00805f9b34fb": "Battery Level",
  "00002a24-0000-1000-8000-00805f9b34fb": "Model Number",
  "00002a26-0000-1000-8000-00805f9b34fb": "Firmware Revision",
  "00002a29-0000-1000-8000-00805f9b34fb": "Manufacturer Name",
  [BENCH_SERVICE]: "Bench",
  [MIDI_SERVICE]: "BLE-MIDI",
};
for (const c of Object.values(CHR)) NAMES[c.uuid] = c.name;

export const nameOf = (uuid: string) => NAMES[uuid] ?? null;

// 0000180f-0000-1000-8000-00805f9b34fb → 180F; our own → ba5e0010; anything else as is.
export function shortUuid(uuid: string): string {
  const m = /^0000([0-9a-f]{4})-0000-1000-8000-00805f9b34fb$/.exec(uuid);
  if (m?.[1]) return m[1].toUpperCase();
  if (uuid.endsWith("-b1e5-4ab1-9e5c-be7cbe7cb0a7")) return uuid.slice(0, 8);
  return uuid.slice(0, 8) + "…";
}
