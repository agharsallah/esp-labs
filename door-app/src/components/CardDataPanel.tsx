import { useState } from "react";
import { api, Inspect, type JobResult, type Snapshot, type Status } from "../api.ts";
import {
  type CompareRow,
  compare,
  describe,
  fileName,
  fromFile,
  isBlank,
  SECTORS,
  toFile,
} from "../cardData.ts";
import { BusyText, Skeleton } from "./Busy.tsx";

type RunJob = (start: () => Promise<number>) => Promise<JobResult>;
type Notify = (text: string, ok: boolean) => void;

interface Props {
  status: Status | null;
  connected: boolean;
  snapshots: Snapshot[];
  loaded: boolean; // the board has sent its saved data at least once
  runJob: RunJob;
  notify: Notify;
}

type Doing = "save" | "copy" | "import" | "cancel";

const MAX = 8;

// What the labs wrote onto a card (blocks 4-6 and 8-10): save it on the board, write it back,
// copy it onto another card, compare it, or keep it as a file. The UID never moves.
export function CardDataPanel({ status, connected, snapshots, loaded, runJob, notify }: Props) {
  const [name, setName] = useState("");
  const [doing, setDoing] = useState<Doing | null>(null);
  const busy = doing !== null;
  const present = status?.present ?? null;
  const waiting = status?.waiting ?? null;
  const copying = waiting?.for === "copy" ? waiting : null;
  const full = snapshots.length >= MAX;

  const act = async (kind: Doing, what: () => Promise<void>) => {
    setDoing(kind);
    try {
      await what();
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setDoing(null);
    }
  };
  const job = (kind: Doing, start: () => Promise<number>, then?: () => void) =>
    act(kind, async () => {
      const r = await runJob(start);
      notify(r.message, r.ok);
      if (r.ok) then?.();
    });

  const importFile = (file: File) =>
    act("import", async () => {
      const parsed = fromFile(await file.text());
      if ("error" in parsed) return notify(`${file.name}: ${parsed.error}`, false);
      const s = await api.importSnapshot(parsed.name, parsed.blocks);
      notify(`“${s.name}” imported: ${s.blocks.length} blocks, ready to write onto a card`, true);
    });

  return (
    <section className="panel data-panel" aria-labelledby="data-title">
      <h2 id="data-title">
        Card data{" "}
        <span className="count">{loaded ? `${snapshots.length} / ${MAX} saved` : "…"}</span>
      </h2>
      <p className="hint data-intro">
        The blocks the labs write: 4-6 and 8-10 (the name label, R5's credits). Block 0, the UID,
        never moves, so a copy is a <b>different card</b> to the door: it still needs its own place
        in the list.
      </p>

      <div className="data-grid">
        <div className="data-actions">
          {copying ? (
            <CopySteps
              from={copying.name}
              uid={copying.uid}
              seconds={Math.ceil(copying.msLeft / 1000)}
              cancelling={doing === "cancel"}
              onCancel={() =>
                act("cancel", async () => {
                  await api.cancel();
                })
              }
            />
          ) : (
            <>
              <form
                className="save-form"
                onSubmit={(e) => {
                  e.preventDefault();
                  job(
                    "save",
                    () => api.save(name.trim()),
                    () => setName(""),
                  );
                }}
              >
                <label>
                  Name for the saved data <span className="opt">(optional)</span>
                  <input
                    value={name}
                    maxLength={23}
                    pattern="[A-Za-z0-9 .'_\-]*"
                    placeholder="the card's name"
                    onChange={(e) => setName(e.target.value)}
                  />
                </label>
                <button
                  type="submit"
                  className="btn primary"
                  disabled={!connected || busy || !present || full}
                  aria-busy={doing === "save"}
                >
                  <BusyText
                    busy={doing === "save"}
                    idle="Save the card on the reader"
                    active="Reading and saving…"
                  />
                </button>
              </form>
              <button
                type="button"
                className="btn copy-btn"
                disabled={!connected || busy || !present || !!waiting}
                aria-busy={doing === "copy"}
                onClick={() => job("copy", api.copy)}
              >
                <BusyText
                  busy={doing === "copy"}
                  idle={
                    <>
                      <CopyGlyph /> Copy this card onto another
                    </>
                  }
                  active="Reading the first card…"
                />
              </button>
              <label className={`btn file-btn ${!connected || busy || full ? "off" : ""}`}>
                <BusyText
                  busy={doing === "import"}
                  idle="Import a saved file"
                  active="Importing…"
                />
                <input
                  type="file"
                  accept="application/json,.json"
                  disabled={!connected || busy || full}
                  onChange={(e) => {
                    const f = e.target.files?.[0];
                    e.target.value = "";
                    if (f) importFile(f);
                  }}
                />
              </label>
            </>
          )}
          <p className="hint">
            {present ? (
              <>
                On the reader: <span className="mono">{present}</span>
              </>
            ) : (
              "Lay a card on the reader to save or copy it."
            )}
            {full && " · All 8 places are used: delete one to save more."}
          </p>
        </div>

        {!loaded ? (
          <Skeleton rows={2} label="Loading the saved card data" />
        ) : snapshots.length === 0 ? (
          <p className="empty">
            Nothing saved yet. Lay a card on the reader and save it, or import a file.
          </p>
        ) : (
          <ul className="snaps">
            {snapshots.map((s) => (
              <SnapshotItem
                key={s.id}
                snap={s}
                present={present}
                disabled={!connected}
                runJob={runJob}
                notify={notify}
              />
            ))}
          </ul>
        )}
      </div>
    </section>
  );
}

// Copy, while the board waits for the second card: step 1 done, step 2 counting down.
function CopySteps(props: {
  from: string;
  uid: string;
  seconds: number;
  cancelling: boolean;
  onCancel: () => void;
}) {
  return (
    <div className="copy-steps" aria-live="polite">
      <div className="copy-cards" aria-hidden="true">
        <span className="card-shape src">
          <span className="chip" />
        </span>
        <span className="flow" />
        <span className="card-shape dst">
          <span className="chip" />
        </span>
      </div>
      <ol>
        <li className="done">
          <span className="step">✓</span>
          <span>
            Read <b>{props.from}</b> <span className="mono">{props.uid}</span>
          </span>
        </li>
        <li className="current">
          <span className="step">2</span>
          <span>
            Lift it, lay the card to copy onto · <b>{props.seconds} s</b>
          </span>
        </li>
        <li>
          <span className="step">3</span>
          <span>The board writes each block and reads it back</span>
        </li>
      </ol>
      <button
        type="button"
        className="btn"
        disabled={props.cancelling}
        aria-busy={props.cancelling}
        onClick={props.onCancel}
      >
        <BusyText busy={props.cancelling} idle="Cancel (nothing is written)" active="Cancelling…" />
      </button>
    </div>
  );
}

function CopyGlyph() {
  return (
    <svg viewBox="0 0 24 16" width="24" height="16" aria-hidden="true" className="glyph">
      <rect x="1" y="2" width="10" height="12" rx="2" />
      <rect x="13" y="2" width="10" height="12" rx="2" />
      <path d="M8 8h8m-2.5-2.5L16 8l-2.5 2.5" />
    </svg>
  );
}

interface ItemProps {
  snap: Snapshot;
  present: string | null;
  disabled: boolean;
  runJob: RunJob;
  notify: Notify;
}

function SnapshotItem({ snap, present, disabled, runJob, notify }: ItemProps) {
  const [confirm, setConfirm] = useState<"write" | "delete" | null>(null);
  const [rows, setRows] = useState<CompareRow[] | null>(null);
  const [doing, setDoing] = useState<"write" | "check" | "delete" | null>(null);
  const busy = doing !== null;
  const saved = new Map(snap.blocks.map((b) => [b.block, b.hex]));
  const same = present !== null && present === snap.uid;

  const act = async (kind: "write" | "check" | "delete", what: () => Promise<void>) => {
    setDoing(kind);
    try {
      await what();
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setDoing(null);
      setConfirm(null);
    }
  };
  const write = () =>
    act("write", async () => {
      const r = await runJob(() => api.restore(snap.id));
      notify(r.message, r.ok);
      setRows(null);
    });
  const check = () =>
    act("check", async () => {
      const r = await runJob(api.inspect);
      if (!r.ok) return notify(r.message, false);
      const card = Inspect.safeParse(r.data);
      if (card.success) setRows(compare(snap.blocks, card.data));
    });
  const download = () => {
    const url = URL.createObjectURL(
      new Blob([JSON.stringify(toFile(snap), null, 2)], { type: "application/json" }),
    );
    const a = document.createElement("a");
    a.href = url;
    a.download = fileName(snap);
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <li className="snap" aria-busy={busy}>
      <div className="snap-head">
        <strong>{snap.name}</strong>
        <span className="snap-from">
          {snap.uid ? (
            <>
              from <span className="mono">{snap.uid}</span>
            </>
          ) : (
            "from a file"
          )}
          {snap.at
            ? ` · ${new Date(snap.at * 1000).toLocaleString(undefined, { dateStyle: "short", timeStyle: "short" })}`
            : ""}
        </span>
      </div>

      <div className="strip" role="list" aria-label="Saved blocks">
        {SECTORS.map((sec) => (
          <div key={sec.sector} className="sector">
            <span className="sector-name">sector {sec.sector}</span>
            {sec.blocks.map((b) => {
              const hex = saved.get(b);
              const kind = hex === undefined ? "none" : isBlank(hex) ? "blank" : "data";
              return (
                <span
                  key={b}
                  role="listitem"
                  className={`cell ${kind}`}
                  title={`block ${b}: ${hex === undefined ? "not saved (didn't open)" : describe(hex)}`}
                >
                  <span className="num">{b}</span>
                  <span className="what">
                    {hex === undefined ? "–" : kind === "blank" ? "0" : describe(hex)}
                  </span>
                </span>
              );
            })}
          </div>
        ))}
      </div>

      <div className="snap-actions">
        {confirm === "write" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy || disabled || !present}
            aria-busy={doing === "write"}
            onClick={write}
          >
            <BusyText
              busy={doing === "write"}
              idle={`Confirm: overwrite ${snap.blocks.map((b) => b.block).join(", ")} on ${present}`}
              active="Writing and reading back…"
            />
          </button>
        ) : (
          <button
            type="button"
            className="btn"
            disabled={busy || disabled || !present}
            title={present ? undefined : "lay the card to write onto on the reader"}
            onClick={() => setConfirm("write")}
          >
            {same ? "Restore onto this card" : "Write onto the card on the reader"}
          </button>
        )}
        <button
          type="button"
          className="btn"
          disabled={busy || disabled || !present}
          aria-busy={doing === "check"}
          onClick={check}
        >
          <BusyText
            busy={doing === "check"}
            idle="Compare with the reader"
            active="Reading the card…"
          />
        </button>
        <button type="button" className="btn" onClick={download}>
          Download
        </button>
        {confirm === "delete" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy || disabled}
            aria-busy={doing === "delete"}
            onClick={() =>
              act("delete", async () => {
                await api.removeSnapshot(snap.id);
                notify(`“${snap.name}” deleted from the board (cards keep their data)`, true);
              })
            }
          >
            <BusyText busy={doing === "delete"} idle="Confirm: delete" active="Deleting…" />
          </button>
        ) : (
          <button
            type="button"
            className="btn ghost"
            disabled={busy || disabled}
            onClick={() => setConfirm("delete")}
          >
            Delete
          </button>
        )}
      </div>

      {rows && (
        <div className="compare">
          <div className="compare-head">
            <span>
              {rows.every((r) => r.match === "same" || r.match === "not saved")
                ? "The card holds this data."
                : `${rows.filter((r) => r.match === "different").length} block(s) differ.`}
            </span>
            <button
              type="button"
              className="btn ghost"
              aria-label="Close"
              onClick={() => setRows(null)}
            >
              ×
            </button>
          </div>
          <table>
            <thead>
              <tr>
                <th>Block</th>
                <th>Saved</th>
                <th>On the card</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {rows.map((r) => (
                <tr key={r.block} className={r.match.replace(" ", "-")}>
                  <td className="mono">{r.block}</td>
                  <td>{r.saved ? describe(r.saved) : "–"}</td>
                  <td>{r.card ? describe(r.card) : r.match === "closed" ? "closed" : "–"}</td>
                  <td className="match">{r.match}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </li>
  );
}
