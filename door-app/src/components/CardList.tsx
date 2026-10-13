import { useState } from "react";
import { api, type Card, type JobResult } from "../api.ts";
import { rulesText, tone } from "../rules.ts";
import { CardEditor } from "./CardEditor.tsx";

interface Props {
  cards: Card[];
  present: string | null;
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
}

export function CardList({ cards, present, runJob, notify }: Props) {
  const [open, setOpen] = useState<string | null>(null);
  const toggleBlock = async (c: Card) => {
    try {
      await api.update(c.uid, { blocked: !c.blocked });
      notify(`${c.name}: ${c.blocked ? "unblocked" : "blocked"}`, true);
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    }
  };

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
              <span className="mono uid">{c.uid}</span>
            </div>
            <span className={`now ${tone(c.now)}`} title="what a tap would do right now">
              {c.now === "granted" ? "opens now" : c.now}
            </span>
            <span className="rules">{rulesText(c)}</span>
            <span className="seen">
              {c.taps} tap{c.taps === 1 ? "" : "s"}
              {c.lastSeen
                ? ` · last ${new Date(c.lastSeen * 1000).toLocaleString(undefined, { dateStyle: "short", timeStyle: "short" })}`
                : ""}
            </span>
            <div className="row-actions">
              <button type="button" className="btn" onClick={() => toggleBlock(c)}>
                {c.blocked ? "Unblock" : "Block"}
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
