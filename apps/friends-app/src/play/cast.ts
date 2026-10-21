// Who stands on the stage: the friends of the last few cards tapped, newest first. Tap card 1
// and card 2 one after the other and both friends play together; a friend nobody tapped for a
// while hops back into its card. Pure functions, so they are easy to test (cast.test.ts).
import type { FriendCard, Show } from "../board/types.ts";

export const MAX_ON_STAGE = 3;
export const STAY_MS = 30_000;

export interface Actor {
  uid: string; // one actor per card
  character: string;
  name: string;
  number: number;
  since: number; // ms, when it was last tapped or shown
  cue: number; // goes up on every show: the stage plays the actor's move again
}

/** A show arrived: the card's friend comes on stage (or plays again). Empty cards don't. */
export function onShow(cast: Actor[], show: Show, now: number): Actor[] {
  if (!show.character || show.full) return cast;
  const old = cast.find((a) => a.uid === show.uid);
  const actor: Actor = {
    uid: show.uid,
    character: show.character,
    name: show.name,
    number: show.number,
    since: now,
    cue: (old?.cue ?? 0) + 1,
  };
  return [actor, ...cast.filter((a) => a.uid !== show.uid)].slice(0, MAX_ON_STAGE);
}

/** Friends whose time is up leave; the newest always stays. */
export function prune(cast: Actor[], now: number): Actor[] {
  const kept = cast.filter((a, i) => i === 0 || now - a.since < STAY_MS);
  return kept.length === cast.length ? cast : kept;
}

/** The card list changed: an actor follows its card's new friend or name, or leaves with it. */
export function follow(cast: Actor[], cards: FriendCard[]): Actor[] {
  let changed = false;
  const next: Actor[] = [];
  for (const a of cast) {
    const card = cards.find((c) => c.uid === a.uid);
    if (!card || !card.character) {
      changed = true;
      continue;
    }
    if (card.character !== a.character || card.name !== a.name) {
      changed = true;
      next.push({ ...a, character: card.character, name: card.name, cue: a.cue + 1 });
    } else next.push(a);
  }
  return changed ? next : cast;
}

/** Where each actor stands, left to right, for 1, 2 or 3 actors (stage units). */
export function spots(count: number): number[] {
  if (count <= 1) return [0];
  if (count === 2) return [-1.15, 1.15];
  return [-2, 0, 2];
}
