// Live state from a board, real or pretend: the status, the card list, and each show as it
// happens. Switching between the real and the pretend board keeps the pretend one's cards.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { liveLink } from "./live.ts";
import { PretendBoard } from "./pretend.ts";
import type { BoardLink, FriendCard, Show, Status } from "./types.ts";

export type BoardMode = "live" | "pretend";

export function useBoard(mode: BoardMode, onShow: (show: Show) => void) {
  const pretend = useRef<PretendBoard | null>(null);
  const link: BoardLink = useMemo(() => {
    if (mode === "live") return liveLink();
    pretend.current ??= new PretendBoard();
    return pretend.current;
  }, [mode]);

  const [status, setStatus] = useState<Status | null>(null);
  const [cards, setCards] = useState<FriendCard[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [connected, setConnected] = useState(false);
  const showRef = useRef(onShow);
  useEffect(() => {
    showRef.current = onShow;
  }, [onShow]);

  useEffect(() => {
    setStatus(null);
    setLoaded(false);
    setConnected(false);
    return link.subscribe((m) => {
      if (m.type === "status") setStatus(m.data);
      else if (m.type === "cards") {
        setCards(m.data);
        setLoaded(true);
      } else showRef.current(m.data);
    }, setConnected);
  }, [link]);

  /** The pretend reader: lay a card on it (pretend mode only). */
  const pretendTap = useCallback(
    (uid?: string) => {
      const board = pretend.current;
      if (mode !== "pretend" || !board) return;
      board.tap(uid ?? board.newUid());
    },
    [mode],
  );

  return { link, status, cards, loaded, connected, pretendTap };
}
