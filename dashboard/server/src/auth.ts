// Who may use the dashboard and who may be the board.
//
// Dashboard (people): when DASHBOARD_KEY is set, /api/* and /ws need a session cookie, which
// POST /api/session hands out for the right key. The cookie holds an HMAC of the key, not the
// key; it is HttpOnly (page scripts can't read it), SameSite=Strict, and Secure behind HTTPS.
// The React app's files stay public: they contain no data, and they draw the sign-in form.
//
// Board: /device needs "Authorization: Bearer <ESP_TOKEN>", the board's own admin token.
import { createHmac, timingSafeEqual } from "node:crypto";
import type { FastifyInstance, FastifyRequest } from "fastify";
import { SessionBody } from "../../shared/protocol.ts";

export const SESSION_COOKIE = "bench_session";
const MAX_AGE_S = 30 * 24 * 3600;

export function sessionValue(key: string): string {
  return createHmac("sha256", key).update("bench-dashboard-session-v1").digest("hex");
}

// Constant time, so response timing doesn't reveal how much of a guess was right.
export function safeEqual(a: string, b: string): boolean {
  const x = Buffer.from(a);
  const y = Buffer.from(b);
  return x.length === y.length && timingSafeEqual(x, y);
}

export function readCookie(header: string | undefined, name: string): string | undefined {
  for (const part of header?.split(";") ?? []) {
    const [k, ...v] = part.trim().split("=");
    if (k === name) return v.join("=");
  }
  return undefined;
}

export function bearerMatches(header: string | undefined, token: string | undefined): boolean {
  return token !== undefined && header !== undefined && safeEqual(header, `Bearer ${token}`);
}

function needsSession(url: string): boolean {
  const path = url.split("?")[0] ?? "";
  return (path.startsWith("/api/") && path !== "/api/session") || path === "/ws";
}

function cookie(req: FastifyRequest, value: string, maxAge: number): string {
  const secure = req.protocol === "https" ? "; Secure" : "";
  return `${SESSION_COOKIE}=${value}; Path=/; HttpOnly; SameSite=Strict; Max-Age=${maxAge}${secure}`;
}

export function registerAuth(app: FastifyInstance, key: string | undefined): void {
  const expected = key ? sessionValue(key) : undefined;
  const signedIn = (req: FastifyRequest) =>
    expected === undefined ||
    safeEqual(readCookie(req.headers.cookie, SESSION_COOKIE) ?? "", expected);

  app.addHook("onRequest", async (req, reply) => {
    if (expected === undefined || !needsSession(req.url) || signedIn(req)) return;
    return reply.code(401).send({ error: "sign in first" });
  });

  app.get("/api/session", async (req) => ({
    required: key !== undefined,
    signedIn: signedIn(req),
  }));

  app.post("/api/session", async (req, reply) => {
    if (key === undefined || expected === undefined) return { ok: true };
    const body = SessionBody.safeParse(req.body);
    if (!body.success || !safeEqual(body.data.key, key)) {
      app.log.warn({ ip: req.ip }, "wrong dashboard key");
      await new Promise((r) => setTimeout(r, 1000)); // slows down guessing
      return reply.code(401).send({ error: "That key is not right." });
    }
    return reply.header("Set-Cookie", cookie(req, expected, MAX_AGE_S)).send({ ok: true });
  });

  app.delete("/api/session", async (req, reply) =>
    reply.header("Set-Cookie", cookie(req, "", 0)).send({ ok: true }),
  );
}
