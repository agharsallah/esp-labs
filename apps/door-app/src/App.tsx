import { useCallback, useState } from "react";
import { BenchBar, ProblemBanner } from "./components/BenchBar.tsx";
import { CardDataPanel } from "./components/CardDataPanel.tsx";
import { CardList } from "./components/CardList.tsx";
import { DoorPanel } from "./components/DoorPanel.tsx";
import { EnrollPanel } from "./components/EnrollPanel.tsx";
import { EventLog } from "./components/EventLog.tsx";
import { useDoor } from "./useDoor.ts";

interface Toast {
  id: number;
  text: string;
  ok: boolean;
}

export function App() {
  const { status, cards, snapshots, events, connected, loaded, runJob } = useDoor();
  const [toasts, setToasts] = useState<Toast[]>([]);
  const notify = useCallback((text: string, ok: boolean) => {
    const id = Date.now() + Math.random();
    setToasts((t) => [...t, { id, text, ok }].slice(-4));
    setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), ok ? 3500 : 7000);
  }, []);

  return (
    <div className="page">
      <header className="top">
        <p className="eyebrow">esp-labs · rfid-lab-r7</p>
        <h1>Bench door</h1>
        <p className="sub">
          The board keeps the list and decides every tap. Your cards only bring their UID.
        </p>
        <BenchBar status={status} connected={connected} />
      </header>
      <ProblemBanner status={status} />

      <main className="grid">
        <DoorPanel
          status={status}
          cards={cards}
          cardsLoaded={loaded.cards}
          connected={connected}
          runJob={runJob}
          notify={notify}
        />
        <section className="panel cards-panel" aria-labelledby="cards-title">
          <h2 id="cards-title">
            Cards <span className="count">{loaded.cards ? `${cards.length} / 20` : "…"}</span>
          </h2>
          <EnrollPanel status={status} connected={connected} runJob={runJob} notify={notify} />
          <CardList
            cards={cards}
            loaded={loaded.cards}
            present={status?.present ?? null}
            waiting={!!status?.waiting}
            runJob={runJob}
            notify={notify}
          />
        </section>
        <section className="panel log-panel" aria-labelledby="log-title">
          <h2 id="log-title">Access log</h2>
          <EventLog events={events} loaded={loaded.events} />
        </section>
        <CardDataPanel
          status={status}
          connected={connected}
          snapshots={snapshots}
          loaded={loaded.snapshots}
          runJob={runJob}
          notify={notify}
        />
      </main>

      <div className="toasts" role="status">
        {toasts.map((t) => (
          <p key={t.id} className={t.ok ? "ok" : "bad"}>
            {t.text}
          </p>
        ))}
      </div>
    </div>
  );
}
