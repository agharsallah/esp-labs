// Card friends: two tabs. Play is for the kids (the 3D stage, the cards, the friends to drag);
// Setup is for the grown-up (which board, its health, the card list, sound and motion).
import { useCallback, useEffect, useRef, useState } from "react";
import type { Show } from "./board/types.ts";
import { useBoard } from "./board/useBoard.ts";
import { friendById, type FriendId } from "./friends/characters.ts";
import { type Actor, follow, onShow, prune } from "./play/cast.ts";
import { PlayTab } from "./play/PlayTab.tsx";
import { useSettings } from "./settings.ts";
import { SetupTab } from "./setup/SetupTab.tsx";
import { playPop, playTune, soundReady, unlock } from "./sound.ts";
import { Toasts, useToasts } from "./ui/Toasts.tsx";

type Tab = "play" | "setup";

export function App() {
  const [settings, change] = useSettings();
  const [tab, setTab] = useState<Tab>("play");
  const [cast, setCast] = useState<Actor[]>([]);
  const [lastShow, setLastShow] = useState<Show | null>(null);
  const [soundOk, setSoundOk] = useState(false);
  const { toasts, notify } = useToasts();
  const soundOn = useRef(settings.sound);
  useEffect(() => {
    soundOn.current = settings.sound;
  }, [settings.sound]);

  // A show from the board: the friend comes on stage with its tune, an empty card asks for one.
  const handleShow = useCallback((show: Show) => {
    setLastShow(show);
    setCast((c) => onShow(c, show, Date.now()));
    if (!soundOn.current) return;
    const friend = friendById(show.character);
    playPop(!friend);
    if (friend) setTimeout(() => playTune(friend), 650);
  }, []);

  const board = useBoard(settings.mode, handleShow);
  const { link, cards } = board;

  useEffect(() => setCast((c) => follow(c, cards)), [cards]);
  useEffect(() => {
    const t = setInterval(() => setCast((c) => prune(c, Date.now())), 2000);
    return () => clearInterval(t);
  }, []);
  useEffect(() => {
    setCast([]);
    setLastShow(null);
  }, [settings.mode]);

  // Sound needs one touch of the page first (browser rule).
  useEffect(() => {
    const first = () => {
      unlock();
      setTimeout(() => setSoundOk(soundReady()), 50);
    };
    window.addEventListener("pointerdown", first);
    return () => window.removeEventListener("pointerdown", first);
  }, []);

  const act = useCallback(
    async (what: string, run: () => Promise<unknown>, done?: string) => {
      try {
        await run();
        if (done) notify(done, true);
      } catch (e) {
        console.warn(`[app] ${what} failed`, e);
        notify(e instanceof Error ? e.message : `${what} didn't work`, false);
      }
    },
    [notify],
  );

  const moveIn = useCallback(
    (uid: string, friend: FriendId) => {
      const card = cards.find((c) => c.uid === uid);
      const name = friendById(friend)?.name ?? friend;
      void act(
        "Moving in",
        async () => {
          await link.update(uid, { character: friend });
          await link.show(uid); // pops out on the stage and on the board's screen
        },
        `${name} moved onto card ${card?.number ?? ""}!`,
      );
    },
    [act, cards, link],
  );

  const pressCard = useCallback(
    (uid: string) => {
      if (settings.mode === "pretend") return board.pretendTap(uid);
      const card = cards.find((c) => c.uid === uid);
      if (!card?.character) {
        notify("Nobody lives on this card yet: drag a friend onto it.", true);
        return;
      }
      void act("Showing", () => link.show(uid));
    },
    [act, board, cards, link, notify, settings.mode],
  );

  const poke = useCallback(
    (uid: string) => {
      const friend = friendById(cast.find((a) => a.uid === uid)?.character ?? "");
      if (friend && settings.sound) playTune(friend);
    },
    [cast, settings.sound],
  );

  return (
    <div className="app">
      <header className="top">
        <div className="brand">
          <span className="logo" aria-hidden>
            <i />
            <i />
            <i />
          </span>
          <div>
            <h1>Card friends</h1>
            <p className="eyebrow">esp-labs · rfid-lab-r8</p>
          </div>
        </div>
        <div className="tabs" role="tablist" aria-label="Card friends">
          {(
            [
              ["play", "Play"],
              ["setup", "Setup"],
            ] as const
          ).map(([id, text]) => (
            <button
              key={id}
              type="button"
              role="tab"
              id={`tab-${id}`}
              aria-selected={tab === id}
              aria-controls={`panel-${id}`}
              className={tab === id ? "on" : ""}
              onClick={() => setTab(id)}
            >
              {text}
            </button>
          ))}
        </div>
        <span className={`link-pill ${board.connected ? "ok" : "bad"}`}>
          {settings.mode === "pretend"
            ? "pretend board"
            : board.connected
              ? "board live"
              : "connecting…"}
        </span>
      </header>

      <main>
        <div role="tabpanel" id="panel-play" aria-labelledby="tab-play" hidden={tab !== "play"}>
          <PlayTab
            mode={settings.mode}
            connected={board.connected}
            loaded={board.loaded}
            cards={cards}
            maxCards={board.status?.maxCards ?? 12}
            cast={cast}
            lastShow={lastShow}
            calm={settings.calm}
            soundHint={settings.sound && !soundOk}
            onPoke={poke}
            onMoveIn={moveIn}
            onCardPress={pressCard}
            onNewPretendCard={() => board.pretendTap()}
            onEnableSound={() => {
              unlock();
              setTimeout(() => setSoundOk(soundReady()), 50);
            }}
          />
        </div>
        <div role="tabpanel" id="panel-setup" aria-labelledby="tab-setup" hidden={tab !== "setup"}>
          <SetupTab
            settings={settings}
            change={change}
            status={board.status}
            connected={board.connected}
            loaded={board.loaded}
            cards={cards}
            act={act}
            update={(uid, patch) => link.update(uid, patch)}
            forget={(uid) => link.forget(uid)}
            show={(uid) => link.show(uid)}
            chipReset={() => link.chipReset()}
          />
        </div>
      </main>
      <Toasts toasts={toasts} />
    </div>
  );
}
