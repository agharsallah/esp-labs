// The cast: who can live on a card. The ids and names are the firmware's
// (labs/rfid-lab-r8/src/characters.h; characters.test.ts checks they agree). The board stores only
// the id; everything else here is how the app shows that friend: colours, its 3D move, its
// sparkles, its tune and what it says.
//
// Colours are this app's design tokens for the 3D scene (three.js reads hsl() strings), so the
// scene and the CSS (via --friend-* custom properties) use the same values.

export type FriendId =
  "fox" | "robot" | "dragon" | "penguin" | "alien" | "cat" | "frog" | "unicorn";

/** The friend's signature move when it jumps out of its card (stage/moves.ts). */
export type Move = "spin" | "dance" | "fly" | "slide" | "beam" | "wiggle" | "leap" | "gallop";

/** What flies around when it appears (stage/effects.ts). */
export type Effect =
  "sparkles" | "bolts" | "fire" | "snow" | "rings" | "hearts" | "bubbles" | "rainbow";

export interface Tune {
  wave: OscillatorType;
  /** Semitones above `base`, one per beat; null is a rest. */
  notes: (number | null)[];
  base: number; // Hz
  beat: number; // seconds
}

export interface Friend {
  id: FriendId;
  name: string;
  what: string; // "a quick little fox"
  hello: string; // what it says when it appears
  trick: string; // what it says when you poke it
  move: Move;
  effect: Effect;
  tune: Tune;
  colors: {
    body: string;
    accent: string;
    belly: string;
    skyTop: string;
    skyBottom: string;
    ground: string;
  };
}

export const FRIENDS: readonly Friend[] = [
  {
    id: "fox",
    name: "Foxy",
    what: "a quick little fox",
    hello: "I can spin faster than the wind!",
    trick: "Whee! Again, again!",
    move: "spin",
    effect: "sparkles",
    tune: { wave: "triangle", notes: [0, 4, 7, 12, 7, 12], base: 523.25, beat: 0.09 },
    colors: {
      body: "hsl(26, 95%, 57%)",
      accent: "hsl(18, 40%, 18%)",
      belly: "hsl(35, 100%, 94%)",
      skyTop: "hsl(28, 90%, 70%)",
      skyBottom: "hsl(340, 70%, 82%)",
      ground: "hsl(95, 45%, 52%)",
    },
  },
  {
    id: "robot",
    name: "Bolt",
    what: "a dancing robot",
    hello: "Beep boop! Let's dance!",
    trick: "Dance mode: ON!",
    move: "dance",
    effect: "bolts",
    tune: { wave: "square", notes: [0, 0, 7, null, 5, 7], base: 392, beat: 0.1 },
    colors: {
      body: "hsl(192, 55%, 64%)",
      accent: "hsl(47, 100%, 58%)",
      belly: "hsl(200, 40%, 92%)",
      skyTop: "hsl(200, 70%, 30%)",
      skyBottom: "hsl(180, 55%, 62%)",
      ground: "hsl(210, 20%, 55%)",
    },
  },
  {
    id: "dragon",
    name: "Ember",
    what: "a tiny dragon",
    hello: "Rawr! Watch me fly!",
    trick: "Puff puff, little flame!",
    move: "fly",
    effect: "fire",
    tune: { wave: "sawtooth", notes: [0, 3, 7, 10, 12, 15], base: 220, beat: 0.1 },
    colors: {
      body: "hsl(130, 50%, 52%)",
      accent: "hsl(8, 90%, 60%)",
      belly: "hsl(52, 85%, 76%)",
      skyTop: "hsl(15, 80%, 45%)",
      skyBottom: "hsl(40, 95%, 70%)",
      ground: "hsl(25, 35%, 45%)",
    },
  },
  {
    id: "penguin",
    name: "Pip",
    what: "a sliding penguin",
    hello: "Brrr, it's snowing! Wheee!",
    trick: "Belly slide!",
    move: "slide",
    effect: "snow",
    tune: { wave: "sine", notes: [12, 7, 9, 4, 7, 12], base: 659.25, beat: 0.11 },
    colors: {
      body: "hsl(222, 45%, 32%)",
      accent: "hsl(35, 100%, 58%)",
      belly: "hsl(210, 60%, 98%)",
      skyTop: "hsl(210, 60%, 55%)",
      skyBottom: "hsl(195, 70%, 88%)",
      ground: "hsl(200, 50%, 94%)",
    },
  },
  {
    id: "alien",
    name: "Zib",
    what: "a friendly alien",
    hello: "Greetings, earth friend!",
    trick: "Zip zap, beam me up!",
    move: "beam",
    effect: "rings",
    tune: { wave: "sine", notes: [0, 12, 5, 17, 10, 22], base: 330, beat: 0.12 },
    colors: {
      body: "hsl(90, 75%, 60%)",
      accent: "hsl(270, 70%, 65%)",
      belly: "hsl(95, 90%, 85%)",
      skyTop: "hsl(260, 60%, 18%)",
      skyBottom: "hsl(285, 55%, 45%)",
      ground: "hsl(270, 25%, 40%)",
    },
  },
  {
    id: "cat",
    name: "Mochi",
    what: "a cuddly cat",
    hello: "Purr... I love you!",
    trick: "Mrrrow! That tickles!",
    move: "wiggle",
    effect: "hearts",
    tune: { wave: "triangle", notes: [7, 4, 7, 12, null, 11], base: 587.33, beat: 0.12 },
    colors: {
      body: "hsl(335, 90%, 80%)",
      accent: "hsl(330, 35%, 35%)",
      belly: "hsl(340, 100%, 96%)",
      skyTop: "hsl(320, 70%, 75%)",
      skyBottom: "hsl(45, 100%, 88%)",
      ground: "hsl(150, 40%, 70%)",
    },
  },
  {
    id: "frog",
    name: "Hopper",
    what: "a bouncy frog",
    hello: "Ribbit! How high can I jump?",
    trick: "Boing boing boing!",
    move: "leap",
    effect: "bubbles",
    tune: { wave: "square", notes: [0, 7, 0, 7, 12, 19], base: 261.63, beat: 0.08 },
    colors: {
      body: "hsl(135, 55%, 45%)",
      accent: "hsl(345, 90%, 70%)",
      belly: "hsl(80, 70%, 78%)",
      skyTop: "hsl(185, 60%, 45%)",
      skyBottom: "hsl(160, 55%, 78%)",
      ground: "hsl(195, 55%, 50%)",
    },
  },
  {
    id: "unicorn",
    name: "Luna",
    what: "a magic unicorn",
    hello: "Sparkle time! Make a wish!",
    trick: "Rainbow gallop!",
    move: "gallop",
    effect: "rainbow",
    tune: { wave: "sine", notes: [0, 4, 7, 11, 14, 19], base: 523.25, beat: 0.1 },
    colors: {
      body: "hsl(270, 60%, 97%)",
      accent: "hsl(45, 100%, 60%)",
      belly: "hsl(265, 85%, 80%)",
      skyTop: "hsl(265, 70%, 72%)",
      skyBottom: "hsl(190, 85%, 82%)",
      ground: "hsl(320, 55%, 82%)",
    },
  },
];

const BY_ID = new Map<string, Friend>(FRIENDS.map((f) => [f.id, f]));

/** undefined for "" (nobody lives on the card yet) or an id this app doesn't know. */
export function friendById(id: string): Friend | undefined {
  return BY_ID.get(id);
}

/** The name to show: the card's own name for its friend if it has one, else the friend's. */
export function displayName(friend: Friend, cardName: string): string {
  return cardName.trim() || friend.name;
}

/** CSS custom properties for anything drawn in a friend's colours. */
export function friendStyle(friend: Friend | undefined): Record<string, string> {
  if (!friend) return {};
  return {
    "--friend-body": friend.colors.body,
    "--friend-accent": friend.colors.accent,
    "--friend-belly": friend.colors.belly,
    "--sky-top": friend.colors.skyTop,
    "--sky-bottom": friend.colors.skyBottom,
  };
}
