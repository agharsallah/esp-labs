import type { Status } from "../api.ts";
import { Spinner } from "./Busy.tsx";

// Where a value will be once the first status arrives.
const Pending = () => <span className="ph" aria-label="loading" />;

interface Props {
  status: Status | null;
  connected: boolean;
}

const BAD_STARTS = ["crash (panic)", "watchdog", "brownout (supply dipped)"];

export function ago(seconds: number): string {
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
  const h = Math.floor(seconds / 3600);
  return `${h} h ${Math.floor((seconds % 3600) / 60)} min`;
}

// The bench at a glance, under the title: link to the board, the board itself, the reader chip,
// Wi-Fi and the clock. Each cell turns amber or red with the reason when something is off.
export function BenchBar({ status, connected }: Props) {
  const board = status?.board;
  const reader = status?.reader;
  const badStart = board && BAD_STARTS.includes(board.reset);
  const chipOk = status?.chip.ok ?? true;

  return (
    <dl className="bench-bar" aria-label="Bench status">
      <div className={connected ? "ok" : "bad"}>
        <dt>App ↔ board</dt>
        <dd>
          {connected ? (
            "live"
          ) : (
            <>
              <Spinner /> connecting…
            </>
          )}
        </dd>
      </div>
      <div className={badStart ? "warn" : ""}>
        <dt>Board</dt>
        <dd>
          {board ? `up ${ago(board.uptime)}` : status ? "running" : <Pending />}
          {board && <small>last start: {board.reset}</small>}
        </dd>
      </div>
      <div className={!chipOk ? "bad" : reader?.problem && reader.problemAgo < 300 ? "warn" : ""}>
        <dt>Reader chip</dt>
        <dd>
          {status ? `${status.chip.version}h · ${status.chip.name}` : <Pending />}
          {reader && (
            <small>
              {reader.wait === "irq" ? "IRQ wire" : "polling"}
              {reader.restores > 0 && ` · settings restored ${reader.restores}×`}
              {reader.lost > 0 && ` · lost ${reader.lost}×`}
            </small>
          )}
        </dd>
      </div>
      <div>
        <dt>Wi-Fi</dt>
        <dd className="mono">
          {status ? `${status.wifi.rssi} dBm` : <Pending />}
          {status && <small>{status.wifi.ip}</small>}
        </dd>
      </div>
      <div className={status && !status.clock.synced ? "warn" : ""}>
        <dt>Clock</dt>
        <dd className="mono">
          {status ? (
            status.clock.synced ? (
              new Date(status.clock.now * 1000).toLocaleTimeString()
            ) : (
              "not set"
            )
          ) : (
            <Pending />
          )}
          {status && !status.clock.synced && <small>dated cards are refused</small>}
        </dd>
      </div>
    </dl>
  );
}

// One line on what went wrong lately, with what to do about it. Nothing when all is well.
export function ProblemBanner({ status }: { status: Status | null }) {
  const board = status?.board;
  const reader = status?.reader;
  if (board && BAD_STARTS.includes(board.reset) && board.uptime < 600) {
    return (
      <p className="banner warn" role="status">
        <b>
          The board restarted {ago(board.uptime)} ago after a {board.reset}.
        </b>{" "}
        {board.reset.startsWith("brownout")
          ? "Its supply dipped: try another USB cable or port, and keep the RC522 on the 3.3V pin with short wires."
          : "Paste the serial monitor's last lines (the backtrace) into the chat."}
      </p>
    );
  }
  if (status && !status.chip.ok) {
    return (
      <p className="banner bad" role="status">
        <b>The reader chip isn't answering (VersionReg {status.chip.version}h).</b> Press the
        RC522's header pins in, check 3.3V, GND and MISO (GPIO 18), then the board resets it by
        itself.
      </p>
    );
  }
  if (reader?.problem && reader.problemAgo < 300) {
    return (
      <p className="banner warn" role="status">
        <b>Reader, {ago(reader.problemAgo)} ago:</b> {reader.problem}.
        {reader.problem.includes("3.3V dip") &&
          " If this repeats, the RC522's supply wobbles when Wi-Fi transmits: short wires to the 3.3V pin, header pins pressed in or soldered."}
      </p>
    );
  }
  return null;
}
