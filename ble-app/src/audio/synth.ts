// A tiny WebAudio synth for the Music tab: two detuned oscillators through a soft envelope.
// Browsers only allow sound after a click or key press, so the AudioContext starts on the first one.
import { frequency } from "../protocol/midi.ts";

type Voice = { osc: OscillatorNode[]; gain: GainNode };

export class Synth {
  private ctx: AudioContext | null = null;
  private master: GainNode | null = null;
  private voices = new Map<number, Voice>();
  volume = 0.5;

  private ensure() {
    if (!this.ctx) {
      this.ctx = new AudioContext();
      this.master = this.ctx.createGain();
      this.master.gain.value = this.volume;
      this.master.connect(this.ctx.destination);
    }
    if (this.ctx.state === "suspended") void this.ctx.resume();
    return this.ctx;
  }

  setVolume(v: number) {
    this.volume = v;
    if (this.master) this.master.gain.value = v;
  }

  noteOn(note: number, velocity = 100) {
    const ctx = this.ensure();
    this.noteOff(note);
    const gain = ctx.createGain();
    const peak = 0.25 * (velocity / 127);
    gain.gain.setValueAtTime(0, ctx.currentTime);
    gain.gain.linearRampToValueAtTime(peak, ctx.currentTime + 0.01);
    gain.gain.exponentialRampToValueAtTime(peak * 0.6, ctx.currentTime + 0.3);
    gain.connect(this.master!);
    const osc = [-4, 4].map((detune, i) => {
      const o = ctx.createOscillator();
      o.type = i ? "sawtooth" : "triangle";
      o.frequency.value = frequency(note);
      o.detune.value = detune;
      const g = ctx.createGain();
      g.gain.value = i ? 0.25 : 0.75;
      o.connect(g).connect(gain);
      o.start();
      return o;
    });
    this.voices.set(note, { osc, gain });
  }

  noteOff(note: number) {
    const v = this.voices.get(note);
    if (!v || !this.ctx) return;
    const t = this.ctx.currentTime;
    v.gain.gain.cancelScheduledValues(t);
    v.gain.gain.setTargetAtTime(0, t, 0.08);
    for (const o of v.osc) o.stop(t + 0.5);
    this.voices.delete(note);
  }
}
