import { useState } from "react";
import { api, type Card, type JobResult } from "../api.ts";
import { rulesText, tone } from "../rules.ts";
import { BusyText, Skeleton } from "./Busy.tsx";
import { CardEditor } from "./CardEditor.tsx";

interface Props {
  cards: Card[];
  loaded: boolean; // the board has sent its list at least once
  present: string | null;
  waiting: boolean;
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
}

export function CardList({ cards, loaded, present, waiting, runJob, notify }: Props) {
  const [open, setOpen] = useState<string | null>(null);
  const [blocking, setBlocking] = useState<string | null>(null); // uid whose Block is on its way
  const toggleBlock = async (c: Card) => {
    setBlocking(c.uid);
    try {
      await api.update(c.uid, { blocked: !c.blocked });
      notify(`${c.name}: ${c.blocked ? "unblocked" : "blocked"}`, true);
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setBlocking(null);
    }
  };

  if (!loaded) return <Skeleton rows={3} label="Loading the card list" />;
  if (!cards.length)
    return <p className="empty">No cards yet. Add one: name it, then tap it on the reader.</p>;
  return (
    <ul className="cards">
      {cards.map((c) => (
        <li key={c.uid} className={c.uid === present ? "here" : ""}>
          <div className="card-row">
            <div className="who">
              <strong>{c.name}</strong>
              <span className={`role ${c.role}`}>{c.role}</span>
              {c.uid === present && <span className="here-tag">on the reader</span>}
            </div>
            <span className="opens" title="what a tap would do right now, at each lock">
              <Opens lock="front door" decision={c.now.front} />
              <Opens lock="admin room" decision={c.now.admin} />
            </span>
            <p className="card-meta">
              <span className="mono">{c.uid}</span>
              <span>{rulesText(c)}</span>
              <span>
                {c.taps} tap{c.taps === 1 ? "" : "s"}
                {c.lastSeen
                  ? `, last ${new Date(c.lastSeen * 1000).toLocaleString(undefined, { dateStyle: "short", timeStyle: "short" })}`
                  : ""}
              </span>
            </p>
            <div className="row-actions">
              <button
                type="button"
                className="btn"
                disabled={blocking === c.uid}
                aria-busy={blocking === c.uid}
                onClick={() => toggleBlock(c)}
              >
                <BusyText
                  busy={blocking === c.uid}
                  idle={c.blocked ? "Unblock" : "Block"}
                  active={c.blocked ? "Unblocking…" : "Blocking…"}
                />
              </button>
              <button
                type="button"
                className="btn"
                aria-expanded={open === c.uid}
                onClick={() => setOpen(open === c.uid ? null : c.uid)}
              >
                {open === c.uid ? "Close" : "Edit"}
              </button>
            </div>
          </div>
          {open === c.uid && (
            <CardEditor
              card={c}
              others={cards.filter((o) => o.uid !== c.uid)}
              waiting={waiting}
              present={present === c.uid}
              runJob={runJob}
              notify={notify}
              onClose={() => setOpen(null)}
            />
          )}
        </li>
      ))}
    </ul>
  );
}

// "front door ✓" in green, or "admin room · admins only" in the decision's colour.
function Opens({ lock, decision }: { lock: string; decision: string }) {
  return (
    <span className={`now ${tone(decision)}`}>
      {lock}
      {decision === "granted" ? " ✓" : ` · ${decision}`}
    </span>
  );
}
