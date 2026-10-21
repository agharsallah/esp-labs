import { useState } from "react";
import { api, type JobResult, type Role, type Status } from "../api.ts";
import { BusyText } from "./Busy.tsx";

interface Props {
  status: Status | null;
  connected: boolean;
  runJob: (start: () => Promise<number>) => Promise<JobResult>;
  notify: (text: string, ok: boolean) => void;
}

// Add a card: name it, pick a role, then tap the new card on the reader within 20 s. While the
// board waits for a new card (adding or replacing one), this shows the countdown instead.
export function EnrollPanel({ status, connected, runJob, notify }: Props) {
  const [name, setName] = useState("");
  const [role, setRole] = useState<Role>("resident");
  const [busy, setBusy] = useState(false); // from the click until the board answers
  const [cancelling, setCancelling] = useState(false);
  const waiting = status?.waiting;

  const start = async () => {
    setBusy(true);
    try {
      const r = await runJob(() => api.enroll(name.trim(), role));
      notify(r.ok ? `${name.trim()} added` : r.message, r.ok);
      if (r.ok) setName("");
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e), false);
    } finally {
      setBusy(false);
    }
  };
  const cancel = () => {
    setCancelling(true);
    api
      .cancel()
      .catch((e) => notify(String(e), false))
      .finally(() => setCancelling(false));
  };

  if (waiting && waiting.for !== "copy") {
    return (
      <div className="enroll waiting" aria-live="polite">
        <div className="pulse" aria-hidden="true" />
        <div>
          <strong>
            Tap the {waiting.for === "replace" ? "replacement" : "new"} card for “{waiting.name}”
          </strong>
          <span>
            {Math.ceil(waiting.msLeft / 1000)} s left ·{" "}
            {waiting.for === "replace"
              ? `it takes over the rules, ${waiting.uid} gets blocked`
              : "the board adds the first card it hasn't seen"}
          </span>
        </div>
        <button
          type="button"
          className="btn"
          disabled={cancelling}
          aria-busy={cancelling}
          onClick={cancel}
        >
          <BusyText busy={cancelling} idle="Cancel" active="Cancelling…" />
        </button>
      </div>
    );
  }
  return (
    <form
      className="enroll"
      onSubmit={(e) => {
        e.preventDefault();
        void start();
      }}
    >
      <label>
        New card's name
        <input
          value={name}
          maxLength={23}
          pattern="[A-Za-z0-9 .'_\-]+"
          required
          placeholder="e.g. Anna, Guest room, Spare tag"
          onChange={(e) => setName(e.target.value)}
        />
      </label>
      <label>
        Role
        <select value={role} onChange={(e) => setRole(e.target.value as Role)}>
          <option value="resident">resident</option>
          <option value="guest">guest (starts with 5 uses)</option>
          <option value="maintenance">maintenance (opens in lockdown)</option>
          <option value="admin">admin (opens the admin room, and in lockdown)</option>
        </select>
      </label>
      <button
        type="submit"
        className="btn primary"
        disabled={!connected || !name.trim() || !!waiting || busy}
        aria-busy={busy}
        title={waiting ? "the board is waiting for a card to copy onto" : undefined}
      >
        <BusyText busy={busy} idle="Add card" active="Starting…" />
      </button>
    </form>
  );
}
