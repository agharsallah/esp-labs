import { describe, expect, it } from "vitest";
import { EspMessage, LedBody, SettingsBody, WifiBody } from "../../shared/protocol.ts";
import { Hub } from "../src/hub.ts";

describe("request bodies", () => {
  it("rejects LED values outside 0-255", () => {
    expect(LedBody.safeParse({ r: 0, g: 256, b: 0 }).success).toBe(false);
    expect(LedBody.safeParse({ r: 0, g: 255, b: 0 }).success).toBe(true);
  });

  it("needs at least one setting", () => {
    expect(SettingsBody.safeParse({}).success).toBe(false);
    expect(SettingsBody.safeParse({ backlight: 120 }).success).toBe(true);
  });

  it("allows an empty password (open network) but not a short one", () => {
    expect(WifiBody.safeParse({ ssid: "home", password: "" }).success).toBe(true);
    expect(WifiBody.safeParse({ ssid: "home", password: "short" }).success).toBe(false);
  });
});

describe("Hub history", () => {
  it("keeps only the newest points", () => {
    const hub = new Hub(2, false);
    const status = (rssi: number) =>
      EspMessage.parse({
        type: "status",
        data: {
          firmware: "wifi-lab-w2",
          uptimeS: 1,
          heapFree: 1000,
          psramFree: 1000,
          clients: 0,
          wifi: {
            state: "connected",
            ssid: "example",
            hostname: "bench.local",
            rssi,
            reason: "none yet",
          },
          setup: { active: false, ssid: "bench-setup" },
          screen: {
            view: "status",
            views: ["status"],
            autoScan: true,
            backlight: 255,
            message: "",
          },
          led: { r: 0, g: 0, b: 0 },
          scan: { running: false, count: 0, durationMs: 0 },
        },
      });
    for (const rssi of [-40, -50, -60]) hub.accept(status(rssi), 0);
    expect(hub.snapshot().history.map((p) => p.rssi)).toEqual([-50, -60]);
  });
});
