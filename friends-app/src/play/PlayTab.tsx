// Play: the 3D stage on top, the cards below it, the friends on the shelf at the bottom.
// Tap a card on the reader and its friend jumps out; drag a friend onto a card to move in.
import type { CSSProperties } from "react";
import type { BoardMode } from "../board/useBoard.ts";
import type { FriendCard, Show } from "../board/types.ts";
import {
  displayName,
  FRIENDS,
  friendById,
  type FriendId,
  friendStyle,
} from "../friends/characters.ts";
import { Portrait } from "../stage/Portrait.tsx";
import { Stage } from "../stage/Stage.tsx";
import type { Actor } from "./cast.ts";
import { useDragFriend } from "./useDrag.ts";

interface Props {
  mode: BoardMode;
  connected: boolean;
  loaded: boolean;
  cards: FriendCard[];
  maxCards: number;
  cast: Actor[];
  lastShow: Show | null;
  calm: boolean;
  soundHint: boolean;
  onPoke(uid: string): void;
  onMoveIn(uid: string, friend: FriendId): void;
  onCardPress(uid: string): void;
  onNewPretendCard(): void;
  onEnableSound(): void;
}

export function PlayTab(props: Props) {
  const { cards, cast, lastShow, mode } = props;
  const drag = useDragFriend(props.onMoveIn);
  const lead = friendById(cast[0]?.character ?? "");
  const waitingFor = lastShow && !lastShow.character && !lastShow.full ? lastShow.uid : null;

  const pressCard = (uid: string) => {
    if (!drag.dropPicked(uid)) props.onCardPress(uid);
  };

  return (
    <div className={`play${drag.dragging ? " is-dragging" : ""}`}>
      <section className="stage" style={friendStyle(lead) as CSSProperties} aria-label="Stage">
        <Stage
          cast={cast}
          calm={props.calm}
          onPoke={props.onPoke}
          label={
            cast.length
              ? `On stage: ${cast.map((a) => displayName(friendById(a.character) ?? FRIENDS[0]!, a.name)).join(", ")}`
              : "The stage is empty"
          }
        />
        <Bubble key={lastShow?.seq ?? 0} {...props} />
        {props.soundHint && (
          <button type="button" className="sound-hint" onClick={props.onEnableSound}>
            Tap for sound
          </button>
        )}
      </section>

      <section className="dock" aria-labelledby="cards-title">
        <div className="dock-head">
          <h2 id="cards-title">Your cards</h2>
          <p className="hint">
            {drag.picked
              ? `Now tap a card for ${friendById(drag.picked)?.name}.`
              : mode === "pretend"
                ? "Tap a card to lay it on the pretend reader."
                : "Tap a card here to show its friend again."}
          </p>
        </div>
        <ul className="card-row">
          {!props.loaded &&
            [0, 1].map((i) => (
              <li key={i}>
                <span className="tile ph" aria-label="loading" />
              </li>
            ))}
          {cards.map((card) => (
            <li key={card.uid}>
              <CardTile
                card={card}
                over={drag.dragging?.over === card.uid}
                waiting={waitingFor === card.uid}
                onStage={cast.some((a) => a.uid === card.uid)}
                picking={!!drag.picked}
                onPress={() => pressCard(card.uid)}
              />
            </li>
          ))}
          {mode === "pretend" && cards.length < props.maxCards && (
            <li>
              <button type="button" className="tile new" onClick={props.onNewPretendCard}>
                <span className="plus" aria-hidden>
                  +
                </span>
                <span>New card</span>
              </button>
            </li>
          )}
          {props.loaded && !cards.length && mode === "live" && (
            <li className="empty">Tap a card on the reader: it shows up here.</li>
          )}
        </ul>
      </section>

      <section className="shelf" aria-labelledby="shelf-title">
        <div className="dock-head">
          <h2 id="shelf-title">Friends</h2>
          <p className="hint">Drag a friend onto a card, or tap a friend and then a card.</p>
        </div>
        <ul className="friend-grid">
          {FRIENDS.map((f) => (
            <li key={f.id}>
              <button
                type="button"
                className={`friend-chip${drag.picked === f.id ? " picked" : ""}`}
                style={friendStyle(f) as CSSProperties}
                aria-pressed={drag.picked === f.id}
                aria-label={`${f.name}, ${f.what}`}
                onPointerDown={(e) => drag.pointerDown(f.id, e)}
                onPointerMove={drag.pointerMove}
                onPointerUp={drag.pointerUp}
                onPointerCancel={drag.cancel}
                onKeyDown={(e) => drag.keyPick(f.id, e)}
              >
                <Portrait friend={f} />
                <span className="chip-name">{f.name}</span>
                <span className="chip-what">{f.what}</span>
              </button>
            </li>
          ))}
        </ul>
      </section>

      {drag.dragging && (
        <div className="ghost" style={{ left: drag.dragging.x, top: drag.dragging.y }} aria-hidden>
          <Portrait friend={friendById(drag.dragging.friend) ?? FRIENDS[0]!} />
        </div>
      )}
    </div>
  );
}

function CardTile({
  card,
  over,
  waiting,
  onStage,
  picking,
  onPress,
}: {
  card: FriendCard;
  over: boolean;
  waiting: boolean;
  onStage: boolean;
  picking: boolean;
  onPress(): void;
}) {
  const friend = friendById(card.character);
  const classes = [
    "tile",
    over && "over",
    waiting && "waiting",
    onStage && "on-stage",
    picking && "can-drop",
  ]
    .filter(Boolean)
    .join(" ");
  return (
    <button
      type="button"
      className={classes}
      data-drop-uid={card.uid}
      style={friendStyle(friend) as CSSProperties}
      onClick={onPress}
      aria-label={`Card ${card.number}: ${friend ? displayName(friend, card.name) : "nobody lives here yet"}`}
    >
      <span className="tile-number">{card.number}</span>
      {friend ? (
        <Portrait friend={friend} />
      ) : (
        <span className="tile-empty" aria-hidden>
          ?
        </span>
      )}
      <span className="tile-name">
        {friend ? displayName(friend, card.name) : "Who lives here?"}
      </span>
      <span className="tile-visits">
        {card.taps} visit{card.taps === 1 ? "" : "s"}
      </span>
    </button>
  );
}

/** What the stage says: the latest show, or how to start. */
function Bubble({ lastShow, mode, connected, cards }: Props) {
  let title: string;
  let line: string;
  let tone = "";
  if (!lastShow) {
    if (mode === "live" && !connected) {
      title = "Waking up the board…";
      line = "Is the ESP32 on? You can also play with a pretend board in Setup.";
    } else if (!cards.length) {
      title = "Tap a card!";
      line = "Lay a card on the reader and a friend will pop out.";
    } else {
      title = "Tap a card!";
      line =
        mode === "pretend"
          ? "Tap a card below to lay it on the pretend reader."
          : "Lay a card on the reader.";
    }
  } else if (lastShow.full) {
    title = "So many cards!";
    line = "12 cards is the most. Forget one in Setup to make room.";
    tone = "warn";
  } else {
    const friend = friendById(lastShow.character);
    if (!friend) {
      title = lastShow.isNew ? `New card ${lastShow.number}!` : `Card ${lastShow.number}`;
      line = "Who lives here? Drag a friend onto it.";
      tone = "ask";
    } else {
      title = `${lastShow.isNew ? "Hello" : "Hi"}, I'm ${displayName(friend, lastShow.name)}!`;
      line = friend.hello;
    }
  }
  return (
    <div className={`bubble ${tone}`} role="status" aria-live="polite">
      <b>{title}</b>
      <span>{line}</span>
      {lastShow && !lastShow.full && lastShow.character && (
        <small>
          card {lastShow.number}
          {!lastShow.fromApp && ` · visit ${lastShow.taps}`}
        </small>
      )}
    </div>
  );
}
