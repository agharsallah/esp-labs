import type { LogEntry } from "../ble/link.ts";

/** The board's colour language: blue write, violet notify, teal read, amber subscribe, coral trouble. */
export function opTone(
  e: Pick<LogEntry, "dir" | "op">,
): "write" | "notify" | "read" | "sub" | "bad" | "info" {
  if (e.dir === "error") return "bad";
  if (e.op.startsWith("write")) return "write";
  if (e.op === "notify") return "notify";
  if (e.op === "read") return "read";
  if (e.op.includes("subscribe")) return "sub";
  return "info";
}
