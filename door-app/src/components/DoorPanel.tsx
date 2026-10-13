import { useState } from "react";
import {
  api,
  type Card,
  Inspect,
  type Inspect as InspectData,
  type JobResult,
  type Status,
} from "../api.ts";
import { tone } from "../rules.ts";

interface Props {
  status: Status | null;
  cards: Card[];
  connected: boolean;
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
}

// The door as a lamp ring around a padlock (grey locked, green open with a countdown, red
// denied), then what lies on the reader right now, with what you can do with it.
export function DoorPanel({ status, cards, connected, runJob, notify }: Props) {
  const [inspect, setInspect] = useState<InspectData | null>(null);
  const [failed, setFailed] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const mode = status?.door.mode ?? "locked";
  const seconds = status ? Math.ceil(status.door.msLeft / 1000) : 0;
  const present = status?.present ?? null;
  const listed = present ? cards.find((c) => c.uid === present) : undefined;

  const act = async (what: string, run: () => Promise<void>) => {
    setBusy(what);
    setFailed(null);
    try {
      await run();
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setBusy(null);
    }
  };
  const read = () =>
    act("read", async () => {
      const r = await runJob(api.inspect);
      if (!r.ok) return setFailed(r.message);
      const parsed = Inspect.safeParse(r.data);
      setInspect(parsed.success ? parsed.data : null);
    });
  const save = () =>
    act("save", async () => {
      const r = await runJob(() => api.save(listed?.name ?? ""));
      if (!r.ok) return setFailed(r.message);
      notify(r.message, true);
    });

  return (
    <section className="door" data-mode={mode} aria-labelledby="door-title">
      <h2 id="door-title">Bench door</h2>
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
          disabled={!connected || !!busy}
          onClick={() => act("open", async () => notify((await runJob(api.open)).message, true))}
        >
          Open for 5 s
        </button>
        <label className="switch">
          <input
            type="checkbox"
            checked={status?.lockdown ?? false}
            disabled={!connected || !!busy}
            onChange={(e) => act("lockdown", () => api.lockdown(e.target.checked))}
          />
          <span>Lockdown</span>
        </label>
      </div>

      <div className={`slot ${present ? "on" : ""}`} aria-live="polite">
        <span className="slot-card" aria-hidden="true">
          <span className="chip" />
        </span>
        <div className="slot-text">
          <span className="slot-label">On the reader</span>
          {present ? (
            <>
              <strong>{listed ? listed.name : "a card not in the list"}</strong>
              <span className="slot-meta">
                <span className="mono">{present}</span>
                {listed && (
                  <span className={`now ${tone(listed.now)}`}>
                    {listed.now === "granted" ? "opens now" : listed.now}
                  </span>
                )}
              </span>
            </>
          ) : (
            <>
              <strong>nothing</strong>
              <span className="slot-meta">lay a card or tag flat on the RC522</span>
            </>
          )}
        </div>
        <div className="slot-actions">
          <button
            type="button"
            className="btn"
            disabled={!connected || !!busy || !present}
            onClick={read}
          >
            {busy === "read" ? "Reading…" : "Read it"}
          </button>
          <button
            type="button"
            className="btn"
            disabled={!connected || !!busy || !present}
            onClick={save}
          >
            {busy === "save" ? "Saving…" : "Save its data"}
          </button>
        </div>
        {failed && (
          <p className="slot-error" role="alert">
            {failed}
          </p>
        )}
      </div>

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
