import { describe, expect, it } from "vitest";
import { bearerMatches, readCookie } from "../src/auth.ts";
import { loadConfig } from "../src/config.ts";

const TOKEN = "0123456789abcdef0123456789abcdef";

describe("sign-in helpers", () => {
  it("reads one cookie out of the header", () => {
    expect(readCookie("a=1; bench_session=abc=; b=2", "bench_session")).toBe("abc=");
    expect(readCookie(undefined, "bench_session")).toBeUndefined();
  });

  it("only accepts the board with the exact token", () => {
    expect(bearerMatches(`Bearer ${TOKEN}`, TOKEN)).toBe(true);
    expect(bearerMatches(`Bearer ${TOKEN.slice(1)}x`, TOKEN)).toBe(false);
    expect(bearerMatches(undefined, TOKEN)).toBe(false);
  });
});

describe("relay mode config", () => {
  it("refuses to start without a dashboard key, because the tunnel makes it public", () => {
    expect(() => loadConfig({ ESP_MODE: "relay", ESP_TOKEN: TOKEN })).toThrow("DASHBOARD_KEY");
    expect(
      loadConfig({ ESP_MODE: "relay", ESP_TOKEN: TOKEN, DASHBOARD_KEY: "x".repeat(16) }).mode,
    ).toBe("relay");
  });
});

describe("hosted server config", () => {
  it("reads TRUST_PROXY as a flag or a list", () => {
    const at = (TRUST_PROXY: string) => loadConfig({ TRUST_PROXY }).trustProxy;
    expect(at("true")).toBe(true);
    expect(at("127.0.0.1, 10.0.0.0/8")).toEqual(["127.0.0.1", "10.0.0.0/8"]);
  });
});
