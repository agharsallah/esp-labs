import { useState } from "react";
import { api, type Inspect as InspectData, Inspect, type JobResult, type Status } from "../api.ts";

interface Props {
  status: Status | null;
  connected: boolean;
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
}

// The door as a lamp ring around a padlock: grey locked, green open (with a countdown), red denied.
export function DoorPanel({ status, connected, runJob, notify }: Props) {
  const [inspect, setInspect] = useState<InspectData | null>(null);
  const [busy, setBusy] = useState(false);
  const mode = status?.door.mode ?? "locked";
  const seconds = status ? Math.ceil(status.door.msLeft / 1000) : 0;

  const act = async (what: () => Promise<void>) => {
    setBusy(true);
    try {
      await what();
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setBusy(false);
    }
  };
  const read = () =>
    act(async () => {
      const r = await runJob(api.inspect);
      if (!r.ok) return notify(r.message, false);
      const parsed = Inspect.safeParse(r.data);
      setInspect(parsed.success ? parsed.data : null);
    });

  return (
    <section className="door" data-mode={mode} aria-labelledby="door-title">
      <div className="door-head">
        <h2 id="door-title">Bench door</h2>
        <span className={`link ${connected ? "on" : ""}`}>
          {connected ? "live" : "connecting…"}
        </span>
      </div>
      <div className="lamp" aria-live="polite">
        <svg viewBox="0 0 120 120" role="img" aria-label={`Door ${mode}`}>
          <circle className="ring" cx="60" cy="60" r="54" />
          <g className="lock">
            <path className="shackle" d="M44 56 V42 a16 16 0 0 1 32 0 V56" />
            <rect className="body" x="36" y="54" width="48" height="38" rx="7" />
            <circle className="hole" cx="60" cy="70" r="5" />
            <rect className="hole" x="58" y="72" width="4" height="10" />
          </g>
        </svg>
        <div className="lamp-text">
          <strong>{mode === "open" ? "OPEN" : mode === "denied" ? "DENIED" : "LOCKED"}</strong>
          <span>
            {mode === "open" && `${status?.door.name || "?"} · ${seconds} s`}
            {mode === "denied" && `${status?.door.why} · ${status?.door.name || status?.door.uid}`}
            {mode === "locked" &&
              (status?.lockdown
                ? "lockdown: maintenance cards only"
                : `${status?.cards ?? 0} cards in the list`)}
          </span>
        </div>
      </div>

      <div className="door-actions">
        <button
          type="button"
          className="btn primary"
          disabled={!connected || busy}
          onClick={() => act(async () => notify((await runJob(api.open)).message, true))}
        >
          Open for 5 s
        </button>
        <label className="switch">
          <input
            type="checkbox"
            checked={status?.lockdown ?? false}
            disabled={!connected || busy}
            onChange={(e) => act(() => api.lockdown(e.target.checked))}
          />
          <span>Lockdown</span>
        </label>
        <button type="button" className="btn" disabled={!connected || busy} onClick={read}>
          Read the card on the reader
        </button>
      </div>

      <dl className="facts">
        <div>
          <dt>Reader chip</dt>
          <dd className={status && !status.chip.ok ? "bad" : ""}>
            {status ? `${status.chip.version}h · ${status.chip.name}` : "–"}
          </dd>
        </div>
        <div>
          <dt>On the reader</dt>
          <dd className="mono">{status?.present ?? "nothing"}</dd>
        </div>
        <div>
          <dt>Board</dt>
          <dd className="mono">{status ? `${status.wifi.ip} · ${status.wifi.rssi} dBm` : "–"}</dd>
        </div>
        <div>
          <dt>Clock</dt>
          <dd className={status && !status.clock.synced ? "bad" : ""}>
            {status
              ? status.clock.synced
                ? new Date(status.clock.now * 1000).toLocaleTimeString()
                : "not set: dated cards are refused"
              : "–"}
          </dd>
        </div>
      </dl>

      {inspect && (
        <div className="inspect">
          <div className="inspect-head">
            <strong className="mono">{inspect.uid}</strong>
            <span>
              {inspect.type} ·{" "}
              {inspect.listed ? `in the list as ${inspect.name}` : "not in the list"}
            </span>
            <button
              type="button"
              className="btn ghost"
              onClick={() => setInspect(null)}
              aria-label="Close"
            >
              ×
            </button>
          </div>
          <table>
            <thead>
              <tr>
                <th>Block</th>
                <th>Bytes</th>
                <th>Text</th>
              </tr>
            </thead>
            <tbody>
              {inspect.blocks.map((b) => (
                <tr key={b.block}>
                  <td className="mono">{b.block}</td>
                  <td className="mono hex">
                    {b.closed ? "closed (not the factory key)" : b.hex?.replace(/(..)/g, "$1 ")}
                  </td>
                  <td className="mono">{b.value !== undefined ? `value ${b.value}` : b.text}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="hint">
            Blocks 4-5 hold the label the app writes; 8-9 the credits from R5. The door never reads
            them: it decides from its own list.
          </p>
        </div>
      )}
    </section>
  );
}
