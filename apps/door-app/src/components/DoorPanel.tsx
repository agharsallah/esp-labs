import { useState } from "react";
import {
  api,
  type Card,
  Inspect,
  type Inspect as InspectData,
  type JobResult,
  type LockId,
  type Status,
} from "../api.ts";
import { tone } from "../rules.ts";
import { BusyText, Spinner } from "./Busy.tsx";

interface Props {
  status: Status | null;
  cards: Card[];
  cardsLoaded: boolean; // until the list arrives, a card on the reader can't be named
  connected: boolean;
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
}

type LockView = Status["locks"][number];

const LOCKS: LockId[] = ["front", "admin"];
const TITLE: Record<LockId, string> = { front: "Front door", admin: "Admin room" };
const WHO: Record<LockId, string> = { front: "every role", admin: "admin cards only" };

// The two locks, each a lamp ring around a padlock (grey locked, green open with a countdown,
// red denied); the one reader sits at one of them. Then what lies on the reader right now.
export function DoorPanel({ status, cards, cardsLoaded, connected, runJob, notify }: Props) {
  const [inspect, setInspect] = useState<InspectData | null>(null);
  const [failed, setFailed] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const readerAt = status?.readerAt ?? "front";
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
    <section className="door" aria-labelledby="door-title">
      <h2 id="door-title">Locks</h2>
      <div className="locks">
        {LOCKS.map((id) => (
          <LockTile
            key={id}
            id={id}
            lock={status?.locks.find((l) => l.id === id)}
            here={readerAt === id}
            disabled={!connected || !!busy}
            busy={busy}
            onOpen={() =>
              act(`open-${id}`, async () => {
                const r = await runJob(() => api.open(id));
                notify(r.message, r.ok);
              })
            }
            onMove={() =>
              act(`move-${id}`, async () => {
                await api.readerAt(id);
                notify(`The reader is now at the ${TITLE[id].toLowerCase()}`, true);
              })
            }
          />
        ))}
      </div>

      <div className="door-actions">
        <label className="switch">
          <input
            type="checkbox"
            checked={status?.lockdown ?? false}
            disabled={!connected || !!busy}
            onChange={(e) => act("lockdown", () => api.lockdown(e.target.checked))}
          />
          <span>Lockdown</span>
          {busy === "lockdown" && <Spinner />}
        </label>
        <span className="hint">
          {status?.lockdown
            ? "only maintenance and admin cards open"
            : `${status?.cards ?? 0} cards in the list`}
        </span>
      </div>

      <div className={`slot ${present ? "on" : ""}`} aria-live="polite">
        <span className="slot-card" aria-hidden="true">
          <span className="chip" />
        </span>
        <div className="slot-text">
          <span className="slot-label">
            On the reader
            {status?.busy && (
              <span className="slot-busy">
                <Spinner /> the reader is busy with a job
              </span>
            )}
          </span>
          {present ? (
            <>
              <strong>
                {listed ? listed.name : cardsLoaded ? "a card not in the list" : "a card…"}
              </strong>
              <span className="slot-meta">
                <span className="mono">{present}</span>
                {listed && (
                  <span
                    className={`now ${tone(listed.now[readerAt])}`}
                    title={`a tap at the ${TITLE[readerAt].toLowerCase()}, where the reader is`}
                  >
                    {listed.now[readerAt] === "granted"
                      ? `opens the ${TITLE[readerAt].toLowerCase()}`
                      : listed.now[readerAt]}
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
            aria-busy={busy === "read"}
            onClick={read}
          >
            <BusyText busy={busy === "read"} idle="Read it" active="Reading…" />
          </button>
          <button
            type="button"
            className="btn"
            disabled={!connected || !!busy || !present}
            aria-busy={busy === "save"}
            onClick={save}
          >
            <BusyText busy={busy === "save"} idle="Save its data" active="Saving…" />
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

interface TileProps {
  id: LockId;
  lock: LockView | undefined;
  here: boolean; // the reader is mounted at this lock
  disabled: boolean;
  busy: string | null;
  onOpen: () => void;
  onMove: () => void;
}

function LockTile({ id, lock, here, disabled, busy, onOpen, onMove }: TileProps) {
  const mode = lock?.mode ?? "locked";
  const seconds = Math.ceil((lock?.msLeft ?? 0) / 1000);
  const who = lock?.name || lock?.uid || "";
  return (
    <article
      className={`lock-tile ${here ? "here" : ""}`}
      data-mode={mode}
      data-lock={id}
      aria-label={`${TITLE[id]}: ${mode}`}
    >
      <header className="lock-head">
        <strong>{TITLE[id]}</strong>
        <span>{WHO[id]}</span>
      </header>
      <div className="lamp" aria-live="polite">
        <svg viewBox="0 0 120 120" aria-hidden="true">
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
            {mode === "open" && `${lock?.name || "?"} · ${seconds} s`}
            {mode === "denied" && `${lock?.why} · ${who}`}
            {mode === "locked" && (lock?.why ? `last: ${who}, ${lock.why}` : "no taps yet")}
          </span>
        </div>
      </div>
      <div className="lock-actions">
        <button
          type="button"
          className="btn primary"
          disabled={disabled}
          aria-busy={busy === `open-${id}`}
          onClick={onOpen}
        >
          <BusyText busy={busy === `open-${id}`} idle="Open 5 s" active="Opening…" />
        </button>
        {here ? (
          <span className="reader-here">
            <span className="reader-glyph" aria-hidden="true" /> reader here
          </span>
        ) : (
          <button
            type="button"
            className="btn ghost"
            disabled={disabled}
            aria-busy={busy === `move-${id}`}
            onClick={onMove}
            title="The bench has one reader: mount it at this lock"
          >
            <BusyText busy={busy === `move-${id}`} idle="Move reader here" active="Moving…" />
          </button>
        )}
      </div>
    </article>
  );
}
