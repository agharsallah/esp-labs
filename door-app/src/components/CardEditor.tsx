import { useState } from "react";
import { api, type Card, type CardPatch, type JobResult, type Role } from "../api.ts";
import { anyTime, diff, fromLocalInput, PRESETS, toLocalInput } from "../rules.ts";

interface Props {
  card: Card;
  present: boolean; // this card is lying on the reader right now
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
  onClose: () => void;
}

const HOURS = Array.from({ length: 24 }, (_, h) => h);

// One card's settings. Presets fill the form; nothing is sent until Save. The card actions at the
// bottom need the card on the reader and ask for a second click before they change it.
export function CardEditor({ card, present, runJob, notify, onClose }: Props) {
  const [draft, setDraft] = useState<CardPatch>({
    name: card.name,
    role: card.role,
    blocked: card.blocked,
    validUntil: card.validUntil,
    fromHour: card.fromHour,
    toHour: card.toHour,
    usesLeft: card.usesLeft,
  });
  const [confirm, setConfirm] = useState<"wipe" | "remove" | null>(null);
  const [busy, setBusy] = useState(false);
  const set = (p: CardPatch) => setDraft((d) => ({ ...d, ...p }));
  const changes = diff(card, draft);
  const hours = { fromHour: draft.fromHour ?? 0, toHour: draft.toHour ?? 0 };

  const act = async (what: () => Promise<void>) => {
    setBusy(true);
    try {
      await what();
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setBusy(false);
      setConfirm(null);
    }
  };
  const job = (start: () => Promise<number>) =>
    act(async () => {
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
      </div>

      <form
        className="fields"
        onSubmit={(e) => {
          e.preventDefault();
          act(async () => {
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
          {(["resident", "guest", "maintenance"] as Role[]).map((r) => (
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
          >
            Save
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
            : "Lay this card on the reader for the two actions on the left."}
        </p>
        <button
          type="button"
          className="btn"
          disabled={busy || !present}
          onClick={() => job(() => api.label(card.uid))}
        >
          Write name label
        </button>
        {confirm === "wipe" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy || !present}
            onClick={() => job(() => api.wipe(card.uid))}
          >
            Confirm: blocks 4-6 and 8-10 to zeros
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
        {confirm === "remove" ? (
          <button
            type="button"
            className="btn danger"
            disabled={busy}
            onClick={() =>
              act(async () => {
                await api.remove(card.uid);
                notify(`${card.name} removed from the list`, true);
                onClose();
              })
            }
          >
            Confirm: remove {card.name}
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
