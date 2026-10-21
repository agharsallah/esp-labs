// Calls to the dashboard server (which validates them and forwards them to the board).
export type Result = { ok: true } | { ok: false; error: string };

export async function send(
  method: "POST" | "PUT" | "DELETE",
  path: string,
  body?: unknown,
): Promise<Result> {
  try {
    const res = await fetch(path, {
      method,
      headers: body === undefined ? undefined : { "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (res.ok) return { ok: true };
    const json = (await res.json().catch(() => ({}))) as { error?: string };
    return { ok: false, error: json.error ?? `HTTP ${res.status}` };
  } catch {
    return { ok: false, error: "The dashboard server is not answering. Is `pnpm dev` running?" };
  }
}

export async function uploadFirmware(file: File): Promise<Result> {
  try {
    const res = await fetch("/api/ota", {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: file,
    });
    if (res.ok) return { ok: true };
    const json = (await res.json().catch(() => ({}))) as { error?: string };
    return { ok: false, error: json.error ?? `HTTP ${res.status}` };
  } catch {
    return { ok: false, error: "Upload failed: the dashboard server is not answering." };
  }
}
