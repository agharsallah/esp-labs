// All configuration comes from environment variables (twelve-factor). In development they are
// read from .env by `node --env-file-if-exists=.env`; copy .env.example to start.
import { z } from "zod";
import type { LinkMode } from "../../shared/protocol.ts";

const optional = z
  .string()
  .optional()
  .transform((t) => (t ? t.trim() : undefined));

const Env = z
  .object({
    // lan: the server dials the board at ESP_URL. relay: the board dials in to /device.
    ESP_MODE: z.enum(["lan", "relay"]).default("lan"),
    ESP_URL: z.url().default("http://bench.local"),
    ESP_TOKEN: optional.refine(
      (t) => t === undefined || /^[0-9a-f]{32}$/.test(t),
      "ESP_TOKEN is 32 hex characters (serial command on the board: token)",
    ),
    // Protects the dashboard when it is reachable from outside (a tunnel). Empty = no sign-in.
    DASHBOARD_KEY: optional.refine(
      (k) => k === undefined || k.length >= 16,
      "DASHBOARD_KEY needs at least 16 characters (try: openssl rand -hex 16)",
    ),
    PORT: z.coerce.number().int().min(1).max(65535).default(8787),
    HOST: z.string().default("127.0.0.1"),
    // Which proxy may say "the visitor used HTTPS" (X-Forwarded-*): the tunnel on this machine by
    // default. Behind a hosting platform's proxy: "true", or its IPs/CIDRs, comma-separated.
    TRUST_PROXY: z.string().default("127.0.0.1"),
    LOG_LEVEL: z.enum(["fatal", "error", "warn", "info", "debug", "trace"]).default("info"),
    NODE_ENV: z.string().default("development"),
  })
  .superRefine((e, ctx) => {
    if (e.ESP_MODE !== "relay") return;
    // The board proves who it is with its token, and the dashboard will be public.
    if (!e.ESP_TOKEN)
      ctx.addIssue({
        code: "custom",
        path: ["ESP_TOKEN"],
        message: "relay mode needs ESP_TOKEN: the board signs in with it",
      });
    if (!e.DASHBOARD_KEY)
      ctx.addIssue({
        code: "custom",
        path: ["DASHBOARD_KEY"],
        message: "relay mode needs DASHBOARD_KEY: the tunnel makes the dashboard public",
      });
  });

export interface Config {
  mode: LinkMode;
  espUrl: string;
  espToken: string | undefined;
  dashboardKey: string | undefined;
  port: number;
  host: string;
  trustProxy: boolean | string[];
  logLevel: string;
  production: boolean;
  historySize: number;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const parsed = Env.safeParse(env);
  if (!parsed.success) {
    const problems = parsed.error.issues
      .map((i) => `  ${i.path.join(".")}: ${i.message}`)
      .join("\n");
    throw new Error(`Invalid configuration (check .env):\n${problems}`);
  }
  const e = parsed.data;
  return {
    mode: e.ESP_MODE,
    espUrl: e.ESP_URL.replace(/\/+$/, ""),
    espToken: e.ESP_TOKEN,
    dashboardKey: e.DASHBOARD_KEY,
    port: e.PORT,
    host: e.HOST,
    trustProxy: parseTrustProxy(e.TRUST_PROXY),
    logLevel: e.LOG_LEVEL,
    production: e.NODE_ENV === "production",
    historySize: 600, // one point per second from the board: 10 minutes
  };
}

function parseTrustProxy(value: string): boolean | string[] {
  if (value === "true") return true;
  if (value === "false") return false;
  return value.split(",").map((v) => v.trim());
}
