import { type CSSProperties, useEffect, useState } from "react";
import { type Friend, friendStyle } from "../friends/characters.ts";
import { type Portraits, portraits } from "./portraits.ts";

let cache: Portraits | null = null;

function usePortraits(): Portraits | null {
  const [p, setP] = useState(cache);
  useEffect(() => {
    if (cache) return;
    let live = true;
    void portraits().then((made) => {
      cache = made;
      if (live) setP(made);
    });
    return () => {
      live = false;
    };
  }, []);
  return p;
}

/** A friend's face: its 3D portrait, or its initial on its colour while that's being drawn.
 *  Its size comes from where it sits (styles.css: tiles, chips, rows, the dragged ghost). */
export function Portrait({ friend }: { friend: Friend }) {
  const all = usePortraits();
  const src = all?.[friend.id];
  return (
    <span className="portrait" style={friendStyle(friend) as CSSProperties}>
      {src ? <img src={src} alt="" draggable={false} /> : <b aria-hidden>{friend.name[0]}</b>}
    </span>
  );
}
