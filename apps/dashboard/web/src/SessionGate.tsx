// Shows the dashboard only after sign-in, when the server asks for a key (DASHBOARD_KEY, set
// whenever the dashboard is reachable through a tunnel). Without a key it renders straight away.
import { type FormEvent, type ReactNode, useEffect, useState } from "react";

type Gate = "checking" | "needs-key" | "open" | "server-down";

export function SessionGate({ children }: { children: (signOut?: () => void) => ReactNode }) {
  const [gate, setGate] = useState<Gate>("checking");
  const [required, setRequired] = useState(false);
  const [key, setKey] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    fetch("/api/session")
      .then((r) => r.json() as Promise<{ required: boolean; signedIn: boolean }>)
      .then((s) => {
        setRequired(s.required);
        setGate(s.signedIn ? "open" : "needs-key");
      })
      .catch(() => setGate("server-down"));
  }, []);

  async function signIn(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError(null);
    const res = await fetch("/api/session", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ key }),
    }).catch(() => undefined);
    setBusy(false);
    if (res?.ok) {
      setKey("");
      setGate("open");
    } else {
      const body = (await res?.json().catch(() => ({}))) as { error?: string } | undefined;
      setError(body?.error ?? "The dashboard server is not answering.");
    }
  }

  async function signOut() {
    await fetch("/api/session", { method: "DELETE" }).catch(() => undefined);
    setGate("needs-key");
  }

  if (gate === "open") return <>{children(required ? signOut : undefined)}</>;
  return (
    <main className="signin">
      <form className="panel" onSubmit={signIn} aria-labelledby="signin-h">
        <p className="eyebrow">esp-labs · bench dashboard</p>
        <h1 id="signin-h">Sign in</h1>
        {gate === "checking" && <p className="sub">Checking…</p>}
        {gate === "server-down" && (
          <p className="sub" role="alert">
            The dashboard server is not answering. Is it running?
          </p>
        )}
        {gate === "needs-key" && (
          <>
            <p className="sub">
              This dashboard is reachable from outside your home. Enter the DASHBOARD_KEY from its
              .env file.
            </p>
            <label>
              Dashboard key
              <input
                type="password"
                autoComplete="current-password"
                value={key}
                onChange={(e) => setKey(e.target.value)}
                required
              />
            </label>
            {error && (
              <p className="form-error" role="alert">
                {error}
              </p>
            )}
            <button type="submit" disabled={busy || key.length === 0}>
              {busy ? "Checking…" : "Sign in"}
            </button>
          </>
        )}
      </form>
    </main>
  );
}
