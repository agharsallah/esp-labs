// Setup, for the grown-up: which board to use (the real one or a pretend one), how it is doing,
// every card with its friend and name, and the play settings (sound, calm motion, day or night).
import { type CSSProperties, useEffect, useState } from "react";
import type { FriendCard, Status } from "../board/types.ts";
import type { BoardMode } from "../board/useBoard.ts";
import { FRIENDS, friendById, friendStyle } from "../friends/characters.ts";
import type { Settings, Theme } from "../settings.ts";
import { Portrait } from "../stage/Portrait.tsx";

interface Props {
  settings: Settings;
  change(patch: Partial<Settings>): void;
  status: Status | null;
  connected: boolean;
  loaded: boolean;
  cards: FriendCard[];
  act(what: string, run: () => Promise<unknown>, done?: string): Promise<void>;
  update(uid: string, patch: { character?: string; name?: string }): Promise<unknown>;
  forget(uid: string): Promise<unknown>;
  show(uid: string): Promise<unknown>;
  chipReset(): Promise<unknown>;
}

function ago(s: number): string {
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.floor(s / 60)} min`;
  return `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`;
}

export function SetupTab(props: Props) {
  const { settings, change, status, connected } = props;
  const live = settings.mode === "live";
  return (
    <div className="setup">
      <section className="panel" aria-labelledby="board-title">
        <h2 id="board-title">Board</h2>
        <Segmented
          label="Which board"
          value={settings.mode}
          options={[
            ["live", "The real board"],
            ["pretend", "Pretend board"],
          ]}
          onChange={(mode) => change({ mode: mode as BoardMode })}
        />
        <p className="hint">
          {live
            ? "The ESP32 with rfid-lab-r8, over your Wi-Fi. Cards and friends live on the board."
            : "A board in this browser, to play without the ESP32. Its cards are gone when you close the tab."}
        </p>
        <dl className="cells">
          <Cell
            label="App ↔ board"
            tone={connected ? "ok" : "bad"}
            value={connected ? "live" : "connecting…"}
          />
          <Cell
            label="Reader chip"
            tone={status && !status.chip.ok ? "bad" : ""}
            value={status ? `${status.chip.version}h · ${status.chip.name}` : "…"}
            small={
              status && live ? (status.reader.wait === "irq" ? "IRQ wire" : "polling") : undefined
            }
          />
          <Cell
            label="Cards"
            value={status ? `${status.cards} of ${status.maxCards}` : "…"}
            small={status?.present ? `on the reader: ${status.present}` : "reader empty"}
          />
          {live && (
            <>
              <Cell
                label="Wi-Fi"
                value={status ? `${status.wifi.rssi} dBm` : "…"}
                small={status?.wifi.ip}
              />
              <Cell
                label="Board"
                value={status ? `up ${ago(status.board.uptime)}` : "…"}
                small={status ? `last start: ${status.board.reset}` : undefined}
              />
            </>
          )}
        </dl>
        {live && status?.reader.problem && status.reader.problemAgo < 300 && (
          <p className="banner warn" role="status">
            <b>Reader, {ago(status.reader.problemAgo)} ago:</b> {status.reader.problem}.
          </p>
        )}
        {live && !connected && (
          <p className="banner" role="status">
            No board yet. Check it shows <code>http://bench.local</code> at the bottom of its
            screen, and that <code>friends-app/.env</code> has the right <code>BENCH_URL</code>. Or
            switch to the pretend board.
          </p>
        )}
        {live && (
          <button
            type="button"
            className="ghost-button"
            disabled={!connected}
            onClick={() =>
              void props.act("Resetting the reader", props.chipReset, "Reader chip reset")
            }
          >
            Reset the reader chip
          </button>
        )}
      </section>

      <section className="panel" aria-labelledby="list-title">
        <h2 id="list-title">
          Cards <span className="count">{props.loaded ? props.cards.length : "…"}</span>
        </h2>
        {props.loaded && !props.cards.length && (
          <p className="hint">No cards yet. Tap one on the reader: it adds itself.</p>
        )}
        <ul className="card-list">
          {props.cards.map((c) => (
            <CardRow key={c.uid} card={c} {...props} />
          ))}
        </ul>
      </section>

      <section className="panel" aria-labelledby="prefs-title">
        <h2 id="prefs-title">Play settings</h2>
        <Toggle
          label="Sound"
          hint="Every friend has its own little tune."
          on={settings.sound}
          onChange={(sound) => change({ sound })}
        />
        <Toggle
          label="Calm motion"
          hint="Fewer sparkles, gentler moves, no camera sway."
          on={settings.calm}
          onChange={(calm) => change({ calm })}
        />
        <Segmented
          label="Colours"
          value={settings.theme}
          options={[
            ["auto", "Like the device"],
            ["day", "Day"],
            ["night", "Night"],
          ]}
          onChange={(theme) => change({ theme: theme as Theme })}
        />
      </section>

      <section className="panel how" aria-labelledby="how-title">
        <h2 id="how-title">How to start</h2>
        <ol>
          <li>
            Flash the firmware: <code>uv run pio run -d labs/rfid-lab-r8 -e rfid-lab-r8 -t upload</code>. It uses the
            Wi-Fi W1 saved.
          </li>
          <li>
            In the serial monitor type <code>token</code>, and put it in{" "}
            <code>esp-labs/apps/friends-app/.env</code> as <code>BENCH_TOKEN=</code>.
          </li>
          <li>
            <code>pnpm install</code> and <code>pnpm dev</code> in <code>esp-labs/apps/friends-app</code>
            , then open <code>http://localhost:5175</code> (a tablet on the same Wi-Fi can use the
            Mac's address).
          </li>
          <li>
            Tap each card on the reader once: it adds itself. Then drag a friend onto it in Play.
          </li>
        </ol>
      </section>
    </div>
  );
}

function CardRow({ card, act, update, forget, show }: Props & { card: FriendCard }) {
  const friend = friendById(card.character);
  const [name, setName] = useState(card.name);
  const [sure, setSure] = useState(false);
  useEffect(() => setName(card.name), [card.name]); // renamed elsewhere (serial, another tablet)
  const saveName = () => {
    const clean = name.trim();
    if (clean === card.name) return;
    if (!/^[A-Za-z0-9 .'_-]{0,15}$/.test(clean)) {
      setName(card.name);
      void act("Saving the name", () =>
        Promise.reject(new Error("Names: up to 15 letters, digits, spaces or - . '")),
      );
      return;
    }
    void act(
      "Saving the name",
      () => update(card.uid, { name: clean }),
      clean ? `Now called ${clean}` : "Back to its own name",
    );
  };
  return (
    <li className="card-row-item" style={friendStyle(friend) as CSSProperties}>
      <span className="tile-number">{card.number}</span>
      {friend ? <Portrait friend={friend} /> : <span className="tile-empty small">?</span>}
      <div className="row-fields">
        <label>
          <span>Friend</span>
          <select
            value={card.character}
            onChange={(e) => {
              const id = e.target.value;
              void act(
                "Moving in",
                async () => {
                  await update(card.uid, { character: id });
                  if (id) await show(card.uid);
                },
                id
                  ? `${friendById(id)?.name} lives on card ${card.number}`
                  : `Card ${card.number} is empty now`,
              );
            }}
          >
            <option value="">nobody yet</option>
            {FRIENDS.map((f) => (
              <option key={f.id} value={f.id}>
                {f.name}, {f.what}
              </option>
            ))}
          </select>
        </label>
        <label>
          <span>Name on this card</span>
          <input
            value={name}
            maxLength={15}
            placeholder={friend?.name ?? "a name"}
            onChange={(e) => setName(e.target.value)}
            onBlur={saveName}
            onKeyDown={(e) => e.key === "Enter" && (e.currentTarget as HTMLInputElement).blur()}
          />
        </label>
        <small className="mono">
          {card.uid} · {card.taps} visit{card.taps === 1 ? "" : "s"}
        </small>
      </div>
      <div className="row-actions">
        <button
          type="button"
          className="ghost-button"
          disabled={!friend}
          onClick={() => void act("Showing", () => show(card.uid))}
        >
          Show
        </button>
        <button
          type="button"
          className={`ghost-button${sure ? " danger" : ""}`}
          onClick={() => {
            if (!sure) {
              setSure(true);
              setTimeout(() => setSure(false), 4000);
              return;
            }
            void act("Forgetting", () => forget(card.uid), `Card ${card.number} forgotten`);
          }}
        >
          {sure ? "Sure? Forget it" : "Forget"}
        </button>
      </div>
    </li>
  );
}

function Cell({
  label,
  value,
  small,
  tone = "",
}: {
  label: string;
  value: string;
  small?: string | undefined;
  tone?: string;
}) {
  return (
    <div className={tone}>
      <dt>{label}</dt>
      <dd>
        {value}
        {small && <small>{small}</small>}
      </dd>
    </div>
  );
}

function Toggle({
  label,
  hint,
  on,
  onChange,
}: {
  label: string;
  hint: string;
  on: boolean;
  onChange(v: boolean): void;
}) {
  return (
    <label className="toggle">
      <input
        type="checkbox"
        role="switch"
        checked={on}
        onChange={(e) => onChange(e.target.checked)}
      />
      <span className="track" aria-hidden />
      <span>
        <b>{label}</b>
        <small>{hint}</small>
      </span>
    </label>
  );
}

function Segmented({
  label,
  value,
  options,
  onChange,
}: {
  label: string;
  value: string;
  options: [string, string][];
  onChange(v: string): void;
}) {
  return (
    <fieldset className="segmented">
      <legend>{label}</legend>
      {options.map(([v, text]) => (
        <label key={v} className={v === value ? "on" : ""}>
          <input
            type="radio"
            name={label}
            value={v}
            checked={v === value}
            onChange={() => onChange(v)}
          />
          {text}
        </label>
      ))}
    </fieldset>
  );
}
