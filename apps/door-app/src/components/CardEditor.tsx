import { useState } from "react";
import { api, type Card, type CardPatch, type JobResult, type Role } from "../api.ts";
import { anyTime, diff, fromLocalInput, PRESETS, rulesOf, toLocalInput } from "../rules.ts";
import { BusyText } from "./Busy.tsx";

type Doing = "save" | "label" | "wipe" | "replace" | "remove";

interface Props {
  card: Card;
  others: Card[]; // the rest of the list, for "Copy rules from"
  waiting: boolean; // the board is already waiting for a card
  present: boolean; // this card is lying on the reader right now
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
  onClose: () => void;
}

const HOURS = Array.from({ length: 24 }, (_, h) => h);

// One card's settings. Presets (or another card's rules) fill the form; nothing is sent until
// Save. The card actions at the bottom ask for a second click before they change anything.
export function CardEditor({ card, others, waiting, present, runJob, notify, onClose }: Props) {
  const [draft, setDraft] = useState<CardPatch>({
    name: card.name,
    role: card.role,
    blocked: card.blocked,
    validUntil: card.validUntil,
    fromHour: card.fromHour,
    toHour: card.toHour,
    usesLeft: card.usesLeft,
  });
  const [confirm, setConfirm] = useState<"wipe" | "remove" | "replace" | null>(null);
  const [doing, setDoing] = useState<Doing | null>(null);
  const busy = doing !== null;
  const set = (p: CardPatch) => setDraft((d) => ({ ...d, ...p }));
  const changes = diff(card, draft);
  const hours = { fromHour: draft.fromHour ?? 0, toHour: draft.toHour ?? 0 };

  const act = async (kind: Doing, what: () => Promise<void>) => {
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
  const job = (kind: Doing, start: () => Promise<number>) =>
    act(kind, async () => {
      const r = await runJob(start);
      notify(r.message, r.ok);
    });

  return (
    <div className="editor" role="region" aria-label={`Edit ${card.name}`}>
      <div className="presets" role="group" aria-label="Scenarios">
        {PRESETS.map((p) => (
          <button
            key={p.key}
            type="button"
            className="chip-btn"
            title={p.explain}
            onClick={() => set(p.patch(new Date()))}
          >
            {p.label}
          </button>
        ))}
        {others.length > 0 && (
          <label className="copy-rules">
            Copy rules from
            <select
              value=""
              onChange={(e) => {
                const from = others.find((o) => o.uid === e.target.value);
                if (from) set(rulesOf(from));
              }}
            >
              <option value="">another card…</option>
              {others.map((o) => (
                <option key={o.uid} value={o.uid}>
                  {o.name} ({o.role})
                </option>
              ))}
            </select>
          </label>
        )}
      </div>

      <form
        className="fields"
        onSubmit={(e) => {
          e.preventDefault();
          act("save", async () => {
            await api.update(card.uid, changes);
            notify(`${draft.name}: saved`, true);
          });
        }}
      >
        <label>
          Name
          <input
            value={draft.name}
            maxLength={23}
            pattern="[A-Za-z0-9 .'_\-]+"
            required
            onChange={(e) => set({ name: e.target.value })}
          />
        </label>
        <fieldset className="seg">
          <legend>Role</legend>
          {(["resident", "guest", "maintenance", "admin"] as Role[]).map((r) => (
            <label key={r}>
              <input
                type="radio"
                name={`role-${card.uid}`}
                checked={draft.role === r}
                onChange={() => set({ role: r })}
              />
              {r}
            </label>
          ))}
        </fieldset>
        <label className="check">
          <input
            type="checkbox"
            checked={draft.blocked}
            onChange={(e) => set({ blocked: e.target.checked })}
          />
          Blocked (refused at once, whatever the other rules say)
        </label>
        <fieldset className="row">
          <legend>Hours</legend>
          <label className="check">
            <input
              type="checkbox"
              checked={anyTime(hours)}
              onChange={(e) =>
                set(e.target.checked ? { fromHour: 0, toHour: 0 } : { fromHour: 8, toHour: 18 })
              }
            />
            any time
          </label>
          {!anyTime(hours) && (
            <>
              <label>
                from
                <select
                  value={hours.fromHour}
                  onChange={(e) => set({ fromHour: Number(e.target.value) })}
                >
                  {HOURS.map((h) => (
                    <option key={h} value={h}>
                      {String(h).padStart(2, "0")}:00
                    </option>
                  ))}
                </select>
              </label>
              <label>
                to
                <select
                  value={hours.toHour}
                  onChange={(e) => set({ toHour: Number(e.target.value) })}
                >
                  {HOURS.map((h) => (
                    <option key={h} value={h}>
                      {String(h).padStart(2, "0")}:00
                    </option>
                  ))}
                </select>
              </label>
            </>
          )}
        </fieldset>
        <fieldset className="row">
          <legend>Valid</legend>
          <label className="check">
            <input
              type="checkbox"
              checked={!draft.validUntil}
              onChange={(e) =>
                set({
                  validUntil: e.target.checked ? 0 : Math.floor(Date.now() / 1000) + 7 * 86_400,
                })
              }
            />
            no end date
          </label>
          {!!draft.validUntil && (
            <label>
              until
              <input
                type="datetime-local"
                value={toLocalInput(draft.validUntil)}
                onChange={(e) => set({ validUntil: fromLocalInput(e.target.value) })}
              />
            </label>
          )}
        </fieldset>
        <fieldset className="row">
          <legend>Uses</legend>
          <label className="check">
            <input
              type="checkbox"
              checked={(draft.usesLeft ?? -1) < 0}
              onChange={(e) => set({ usesLeft: e.target.checked ? -1 : 5 })}
            />
            unlimited
          </label>
          {(draft.usesLeft ?? -1) >= 0 && (
            <label>
              left
              <input
                type="number"
                min={0}
                max={9999}
                value={draft.usesLeft}
                onChange={(e) =>
                  set({ usesLeft: Math.max(0, Math.min(9999, Number(e.target.value))) })
                }
              />
            </label>
          )}
        </fieldset>
        <div className="form-actions">
          <button
            type="submit"
            className="btn primary"
            disabled={busy || Object.keys(changes).length === 0}
            aria-busy={doing === "save"}
          >
            <BusyText busy={doing === "save"} idle="Save" active="Saving…" />
          </button>
          <button type="button" className="btn ghost" onClick={onClose}>
            Close
          </button>
        </div>
      </form>

      <div className="card-actions">
        <p className="hint">
          {present
            ? "This card is on the reader."
            : "Lay this card on the reader to write its label or wipe it."}
        </p>
        <button
          type="button"
          className="btn"
          disabled={busy || !present}
          aria-busy={doing === "label"}
          onClick={() => job("label", () => api.label(card.uid))}
        >
          <BusyText busy={doing === "label"} idle="Write name label" active="Writing…" />
        </button>
        {confirm === "wipe" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy || !present}
            aria-busy={doing === "wipe"}
            onClick={() => job("wipe", () => api.wipe(card.uid))}
          >
            <BusyText
              busy={doing === "wipe"}
              idle="Confirm: blocks 4-6 and 8-10 to zeros"
              active="Wiping…"
            />
          </button>
        ) : (
          <button
            type="button"
            className="btn"
            disabled={busy || !present}
            onClick={() => setConfirm("wipe")}
          >
            Wipe card data
          </button>
        )}
        {confirm === "replace" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy || waiting}
            aria-busy={doing === "replace"}
            onClick={() =>
              act("replace", async () => {
                const r = await runJob(() => api.replace(card.uid));
                notify(r.message, r.ok);
                if (r.ok) onClose();
              })
            }
          >
            <BusyText
              busy={doing === "replace"}
              idle="Confirm: block this card, then tap the new one"
              active="Waiting for the new card…"
            />
          </button>
        ) : (
          <button
            type="button"
            className="btn"
            disabled={busy || waiting}
            title="Lost or broken: a new card takes over this card's name and rules; this one is blocked"
            onClick={() => setConfirm("replace")}
          >
            Replace with a new card
          </button>
        )}
        {confirm === "remove" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy}
            aria-busy={doing === "remove"}
            onClick={() =>
              act("remove", async () => {
                await api.remove(card.uid);
                notify(`${card.name} removed from the list`, true);
                onClose();
              })
            }
          >
            <BusyText
              busy={doing === "remove"}
              idle={`Confirm: remove ${card.name}`}
              active="Removing…"
            />
          </button>
        ) : (
          <button
            type="button"
            className="btn"
            disabled={busy}
            onClick={() => setConfirm("remove")}
          >
            Remove from the list
          </button>
        )}
      </div>
    </div>
  );
}
