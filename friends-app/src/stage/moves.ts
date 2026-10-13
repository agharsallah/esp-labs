// How a friend moves: popping out of its card, its signature move, and hopping back in.
// Each one is a pure function of progress p (0 to 1) that returns an offset from where the friend
// stands, so the stage can play any move on any friend and tests could check any frame.
import type { Move } from "../friends/characters.ts";

export interface Pose {
  x: number;
  y: number;
  z: number;
  turn: number; // around the up axis
  tiltX: number; // forwards (+) / backwards
  tiltZ: number; // sideways
  squash: number; // > 1 wide and short, < 1 tall and thin
  scale: number;
  excite: number; // 0-1: how lively tails, wings and arms are
}

export const REST: Pose = {
  x: 0,
  y: 0,
  z: 0,
  turn: 0,
  tiltX: 0,
  tiltZ: 0,
  squash: 1,
  scale: 1,
  excite: 0,
};

const TAU = Math.PI * 2;
const ease = (p: number) => (p < 0.5 ? 2 * p * p : 1 - (-2 * p + 2) ** 2 / 2);
const bell = (p: number) => Math.sin(Math.PI * Math.min(1, Math.max(0, p)));
const backOut = (p: number) => {
  const c = 1.9;
  const x = p - 1;
  return 1 + (c + 1) * x ** 3 + c * x ** 2;
};
/** A crouch before and a squash after a jump that spans [from, to] of p. */
const landing = (p: number, from: number, to: number) =>
  p < from ? 1 + bell(p / from) * 0.25 : p > to ? 1 + bell((p - to) / (1 - to)) * 0.25 : 0.9;

export const DURATION: Record<Move, number> = {
  spin: 1.5,
  dance: 2.6,
  fly: 2.8,
  slide: 2.6,
  beam: 2.8,
  wiggle: 2.2,
  leap: 1.9,
  gallop: 2.6,
};

export function move(kind: Move, p: number, calm: boolean): Pose {
  const a = calm ? 0.6 : 1; // calm mode: smaller, gentler
  const pose: Pose = { ...REST, excite: bell(p) };
  switch (kind) {
    case "spin": {
      const j = Math.min(1, Math.max(0, (p - 0.15) / 0.7));
      pose.y = bell(j) * 1.2 * a;
      pose.turn = ease(j) * TAU * (calm ? 1 : 2);
      pose.squash = landing(p, 0.15, 0.85);
      break;
    }
    case "dance":
      pose.x = Math.sin(p * TAU * 2) * 0.4 * a;
      pose.y = Math.abs(Math.sin(p * TAU * 4)) * 0.18 * a;
      pose.tiltZ = Math.sin(p * TAU * 4) * 0.18 * a;
      pose.turn = Math.sin(p * TAU) * 0.5;
      break;
    case "fly":
      pose.y = bell(p) * 1.5 * a;
      pose.turn = ease(p) * TAU;
      pose.tiltX = Math.sin(p * TAU * 2) * 0.15;
      pose.excite = Math.min(1, bell(p) * 2);
      break;
    case "slide": {
      const lie = Math.min(1, bell(p) * 2);
      pose.x = Math.sin(p * TAU) * 1.1 * a;
      pose.z = (Math.cos(p * TAU) - 1) * 0.7 * a;
      pose.tiltX = lie * 1.2;
      pose.turn = Math.sin(p * TAU) * 1.2;
      pose.y = lie * 0.15;
      break;
    }
    case "beam":
      pose.y = bell(p) * 1.3 * a + Math.sin(p * TAU * 3) * 0.08;
      pose.turn = ease(p) * TAU * 1.5;
      pose.squash = 1 - bell(p) * 0.08;
      break;
    case "wiggle":
      pose.tiltZ = Math.sin(p * TAU * 5) * 0.28 * (1 - p) * a;
      pose.squash = 1 + Math.sin(p * TAU * 5) * 0.08 * (1 - p);
      pose.y = Math.abs(Math.sin(p * TAU * 2.5)) * 0.2 * a;
      break;
    case "leap": {
      const j = Math.min(1, Math.max(0, (p - 0.22) / 0.6));
      pose.y = bell(j) * 2.1 * a;
      pose.tiltX = calm ? 0 : ease(Math.min(1, Math.max(0, (j - 0.2) / 0.6))) * -TAU;
      pose.squash = landing(p, 0.22, 0.82);
      break;
    }
    case "gallop":
      pose.x = Math.sin(p * TAU) * 1.3 * a;
      pose.y = Math.abs(Math.sin(p * TAU * 3)) * 0.35 * a;
      pose.turn = Math.cos(p * TAU) * 1.1;
      pose.tiltX = Math.sin(p * TAU * 6) * 0.12;
      break;
  }
  return pose;
}

/** Out of the card: up from below with a little overshoot and half a turn. */
export function enter(p: number): Pose {
  const k = backOut(Math.min(1, p));
  return {
    ...REST,
    y: (k - 1) * 0.9,
    scale: Math.max(0.01, k),
    turn: (1 - k) * -Math.PI,
    excite: 1 - p,
  };
}

/** Back into the card. */
export function leave(p: number): Pose {
  const k = 1 - ease(Math.min(1, p));
  return {
    ...REST,
    y: bell(p * 0.5) * 0.4 - (1 - k) * 0.6,
    scale: Math.max(0.01, k),
    turn: p * Math.PI,
  };
}
