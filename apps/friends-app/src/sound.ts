// Every friend's little tune, played with the Web Audio API: no sound files, a few oscillators.
// Browsers only allow sound after the person has touched the page once, so unlock() is called on
// the first pointer press; until then a tap on the reader is silent (the app shows a hint).
import type { Friend } from "./friends/characters.ts";

let audio: AudioContext | null = null;

export function unlock(): void {
  try {
    audio ??= new AudioContext();
    if (audio.state === "suspended") void audio.resume();
  } catch (e) {
    console.warn("[sound] no Web Audio here", e);
  }
}

export function soundReady(): boolean {
  return audio?.state === "running";
}

function note(at: number, hz: number, length: number, wave: OscillatorType, volume: number) {
  if (!audio) return;
  const osc = audio.createOscillator();
  const gain = audio.createGain();
  osc.type = wave;
  osc.frequency.setValueAtTime(hz, at);
  // A soft attack and a quick fade: plucky, never harsh.
  gain.gain.setValueAtTime(0, at);
  gain.gain.linearRampToValueAtTime(volume, at + 0.01);
  gain.gain.exponentialRampToValueAtTime(0.0001, at + length);
  osc.connect(gain).connect(audio.destination);
  osc.start(at);
  osc.stop(at + length + 0.02);
}

/** The friend's tune: one note per beat. */
export function playTune(friend: Friend): void {
  if (!soundReady() || !audio) return;
  const { wave, notes, base, beat } = friend.tune;
  const start = audio.currentTime + 0.02;
  // Square and sawtooth waves sound louder than sine at the same level.
  const volume = wave === "square" || wave === "sawtooth" ? 0.05 : 0.12;
  notes.forEach((n, i) => {
    if (n !== null) note(start + i * beat, base * 2 ** (n / 12), beat * 1.6, wave, volume);
  });
}

/** A quick rising "pop" for a friend jumping out, or a question for an empty card. */
export function playPop(question = false): void {
  if (!soundReady() || !audio) return;
  const t = audio.currentTime + 0.01;
  const osc = audio.createOscillator();
  const gain = audio.createGain();
  osc.type = "sine";
  osc.frequency.setValueAtTime(question ? 500 : 300, t);
  osc.frequency.exponentialRampToValueAtTime(question ? 700 : 900, t + 0.12);
  if (question) osc.frequency.exponentialRampToValueAtTime(450, t + 0.3);
  gain.gain.setValueAtTime(0.15, t);
  gain.gain.exponentialRampToValueAtTime(0.0001, t + (question ? 0.35 : 0.18));
  osc.connect(gain).connect(audio.destination);
  osc.start(t);
  osc.stop(t + 0.4);
}
