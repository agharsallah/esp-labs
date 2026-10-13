// The 3D stage in React: one StageWorld for the life of the component, told who is on stage.
// If the browser has no WebGL, the stage says so and the rest of the app still works.
import { useEffect, useRef, useState } from "react";
import type { Actor } from "../play/cast.ts";
import { StageWorld } from "./world.ts";

interface Props {
  cast: Actor[];
  calm: boolean;
  onPoke(uid: string): void;
  label: string;
}

export function Stage({ cast, calm, onPoke, label }: Props) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const world = useRef<StageWorld | null>(null);
  const pokeRef = useRef(onPoke);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    pokeRef.current = onPoke;
  }, [onPoke]);

  useEffect(() => {
    if (!canvas.current) return;
    try {
      world.current = new StageWorld(canvas.current, {
        calm,
        onPoke: (uid) => pokeRef.current(uid),
      });
    } catch (e) {
      console.error("[stage] 3D isn't available", e);
      setFailed(true);
      return;
    }
    return () => {
      world.current?.dispose();
      world.current = null;
    };
    // calm is applied below; the world is made once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => world.current?.setCalm(calm), [calm]);
  useEffect(() => world.current?.setCast(cast), [cast]);

  return (
    <>
      <canvas ref={canvas} className="stage-canvas" role="img" aria-label={label} />
      {failed && (
        <p className="stage-fallback">
          This browser can't draw 3D. Your friends still show on the cards below.
        </p>
      )}
    </>
  );
}
