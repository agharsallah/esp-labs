import { describe, expect, it } from "vitest";
import { buildEspRequest } from "../src/esp.ts";

const TOKEN = "0123456789abcdef0123456789abcdef";

describe("buildEspRequest", () => {
  it("sends everyday controls without the admin token", () => {
    const req = buildEspRequest("http://bench.local", "led", { r: 1, g: 2, b: 3 }, TOKEN);
    expect(req.url).toBe("http://bench.local/api/led");
    expect(req.init.method).toBe("PUT");
    expect(req.init.headers).not.toHaveProperty("Authorization");
    expect(req.init.body).toBe('{"r":1,"g":2,"b":3}');
  });

  it("adds the token to admin routes only", () => {
    const req = buildEspRequest("http://bench.local", "reboot", undefined, TOKEN);
    expect(req.init.headers).toEqual({ Authorization: `Bearer ${TOKEN}` });
    expect(req.init.body).toBeUndefined();
  });
});
