// Admin actions. The token lives only in the dashboard server's .env; this page never sees it.
// Destructive buttons confirm in place (a second click) instead of a browser dialog.
import { useState } from "react";
import type { Networks, Snapshot } from "../../../shared/protocol.ts";
import { send, uploadFirmware, type Result } from "../api.ts";

function Confirm({
  label,
  confirmLabel,
  onConfirm,
  disabled,
}: {
  label: string;
  confirmLabel: string;
  onConfirm: () => void;
  disabled: boolean;
}) {
  const [armed, setArmed] = useState(false);
  return (
    <button
      type="button"
      className={armed ? "danger armed" : "danger"}
      disabled={disabled}
      onBlur={() => setArmed(false)}
      onClick={() => {
        if (armed) {
          setArmed(false);
          onConfirm();
        } else setArmed(true);
      }}
    >
      {armed ? confirmLabel : label}
    </button>
  );
}

export function AdminPanel({
  snapshot,
  otaPercent,
}: {
  snapshot: Snapshot | null;
  otaPercent: number | null;
}) {
  const [message, setMessage] = useState<{ ok: boolean; text: string } | null>(null);
  const [file, setFile] = useState<File | null>(null);
  const [busy, setBusy] = useState(false);
  const offline = !snapshot?.espConnected;

  const report = (r: Result, done: string) =>
    setMessage(r.ok ? { ok: true, text: done } : { ok: false, text: r.error });

  if (!snapshot?.adminEnabled) {
    return (
      <section className="panel" aria-labelledby="admin-h">
        <h2 id="admin-h">Admin</h2>
        <p className="muted">
          Off. To enable reboot, forget Wi-Fi and firmware updates: type <code>token</code> in the
          board's serial monitor, put the value in <code>esp-labs/apps/dashboard/.env</code> as{" "}
          <code>ESP_TOKEN=…</code>, then restart <code>pnpm dev</code>.
        </p>
      </section>
    );
  }

  return (
    <section className="panel" aria-labelledby="admin-h">
      <h2 id="admin-h">Admin</h2>
      <div className="row">
        <Confirm
          label="Reboot"
          confirmLabel="Click again to reboot"
          disabled={offline}
          onConfirm={async () => report(await send("POST", "/api/reboot"), "Rebooting…")}
        />
        <Confirm
          label="Forget Wi-Fi"
          confirmLabel="Click again: the board leaves your network"
          disabled={offline}
          onConfirm={async () =>
            report(
              await send("DELETE", "/api/wifi"),
              "Forgotten. The board opens its set-up network.",
            )
          }
        />
      </div>

      <h3>Firmware update</h3>
      <form
        className="row"
        onSubmit={async (e) => {
          e.preventDefault();
          if (!file) return;
          setBusy(true);
          setMessage({ ok: true, text: "Uploading…" });
          report(await uploadFirmware(file), "Written and verified. The board reboots into it.");
          setBusy(false);
        }}
      >
        <input
          type="file"
          accept=".bin"
          aria-label="Firmware file"
          onChange={(e) => setFile(e.target.files?.[0] ?? null)}
        />
        <button type="submit" disabled={!file || busy || offline}>
          Upload
        </button>
      </form>
      <p className="muted">
        Build it with <code>uv run pio run -d labs/wifi-lab-w2 -e wifi-lab-w2</code>, then pick{" "}
        <code>.pio/build/wifi-lab-w2/firmware.bin</code>.
      </p>
      {otaPercent !== null && (
        <div
          className="progress"
          role="progressbar"
          aria-valuenow={otaPercent}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-label="Firmware update"
        >
          <span style={{ width: `${otaPercent}%` }} />
        </div>
      )}

      <h3>Change network</h3>
      <WifiForm
        networks={snapshot.networks}
        disabled={offline}
        onDone={(r) => report(r, "Saved. The board is joining it.")}
      />
      {message && (
        <p className={message.ok ? "muted" : "error"} role="status">
          {message.text}
        </p>
      )}
    </section>
  );
}

function WifiForm({
  networks,
  disabled,
  onDone,
}: {
  networks: Networks | null;
  disabled: boolean;
  onDone: (r: Result) => void;
}) {
  const names = [...new Set((networks?.networks ?? []).map((n) => n.ssid).filter(Boolean))];
  const [ssid, setSsid] = useState("");
  const [password, setPassword] = useState("");
  return (
    <form
      className="row"
      onSubmit={async (e) => {
        e.preventDefault();
        onDone(await send("PUT", "/api/wifi", { ssid: ssid || names[0], password }));
        setPassword("");
      }}
    >
      <select aria-label="Network" value={ssid} onChange={(e) => setSsid(e.target.value)}>
        {names.map((n) => (
          <option key={n} value={n}>
            {n}
          </option>
        ))}
      </select>
      <input
        type="password"
        aria-label="Password"
        autoComplete="off"
        value={password}
        placeholder="Password"
        onChange={(e) => setPassword(e.target.value)}
      />
      <button type="submit" disabled={disabled || names.length === 0}>
        Join
      </button>
    </form>
  );
}
