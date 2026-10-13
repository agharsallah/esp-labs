// Moving a friend onto a card, three ways, so it works for everyone:
//   - drag it with a finger or the mouse (pointer events, so touch screens work too)
//   - tap it to pick it up, then tap a card
//   - with the keyboard: Enter on a friend, then Enter on a card
// Cards mark themselves as drop targets with data-drop-uid.
import {
  type KeyboardEvent,
  type PointerEvent,
  useCallback,
  useEffect,
  useRef,
  useState,
} from "react";
import type { FriendId } from "../friends/characters.ts";

export interface Dragging {
  friend: FriendId;
  x: number;
  y: number;
  over: string | null; // the card under the pointer
}

const START_PX = 8; // move this far before a press becomes a drag
const EDGE_PX = 70; // dragging this close to the top or bottom of the screen scrolls the page

function cardAt(x: number, y: number): string | null {
  const el = document.elementFromPoint(x, y);
  const card = el instanceof Element ? el.closest<HTMLElement>("[data-drop-uid]") : null;
  return card?.dataset.dropUid ?? null;
}

export function useDragFriend(onDrop: (uid: string, friend: FriendId) => void) {
  const [dragging, setDragging] = useState<Dragging | null>(null);
  const [picked, setPicked] = useState<FriendId | null>(null);
  const press = useRef<{ friend: FriendId; x: number; y: number; id: number } | null>(null);
  const dropRef = useRef(onDrop);
  useEffect(() => {
    dropRef.current = onDrop;
  }, [onDrop]);

  const pointerDown = useCallback((friend: FriendId, e: PointerEvent<HTMLElement>) => {
    if (e.button !== 0) return;
    press.current = { friend, x: e.clientX, y: e.clientY, id: e.pointerId };
    e.currentTarget.setPointerCapture(e.pointerId);
  }, []);

  const pointerMove = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      const p = press.current;
      if (!p || p.id !== e.pointerId) return;
      if (!dragging && Math.hypot(e.clientX - p.x, e.clientY - p.y) < START_PX) return;
      if (e.clientY < EDGE_PX) window.scrollBy(0, -14);
      else if (e.clientY > window.innerHeight - EDGE_PX) window.scrollBy(0, 14);
      setDragging({
        friend: p.friend,
        x: e.clientX,
        y: e.clientY,
        over: cardAt(e.clientX, e.clientY),
      });
    },
    [dragging],
  );

  const pointerUp = useCallback(
    (e: PointerEvent<HTMLElement>) => {
      const p = press.current;
      press.current = null;
      if (!p || p.id !== e.pointerId) return;
      if (dragging) {
        const uid = cardAt(e.clientX, e.clientY);
        setDragging(null);
        setPicked(null);
        if (uid) dropRef.current(uid, p.friend);
      } else {
        // A tap: pick it up (or put it back down).
        setPicked((now) => (now === p.friend ? null : p.friend));
      }
    },
    [dragging],
  );

  const cancel = useCallback(() => {
    press.current = null;
    setDragging(null);
  }, []);

  /** Keyboard: Enter or Space on a friend picks it up. */
  const keyPick = useCallback((friend: FriendId, e: KeyboardEvent) => {
    if (e.key !== "Enter" && e.key !== " ") return;
    e.preventDefault();
    setPicked((now) => (now === friend ? null : friend));
  }, []);

  /** A card was chosen while a friend is picked up: drop it there. true when it did. */
  const dropPicked = useCallback(
    (uid: string) => {
      if (!picked) return false;
      dropRef.current(uid, picked);
      setPicked(null);
      return true;
    },
    [picked],
  );

  useEffect(() => {
    if (!picked) return;
    const esc = (e: globalThis.KeyboardEvent) => e.key === "Escape" && setPicked(null);
    window.addEventListener("keydown", esc);
    return () => window.removeEventListener("keydown", esc);
  }, [picked]);

  return { dragging, picked, pointerDown, pointerMove, pointerUp, cancel, keyPick, dropPicked };
}
