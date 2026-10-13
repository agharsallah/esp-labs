// What flies around a friend when it appears: sparkles, sparks, fire, snow, rings, hearts,
// bubbles, a rainbow, and a puff of smoke for the pop out of the card. Each burst is one
// THREE.Points cloud with a small canvas-drawn picture per particle; it fades and is removed when
// its particles are done. Calm mode sends fewer of them.
import * as THREE from "three";
import type { Effect, Friend } from "../friends/characters.ts";

type Shape = "star" | "heart" | "dot" | "ring" | "flake";

interface Recipe {
  shape: Shape;
  count: number;
  life: number; // seconds
  size: number;
  gravity: number; // + falls, - rises
  sway: number; // side-to-side drift
  spawn(i: number, at: THREE.Vector3, mouth: THREE.Vector3): { p: THREE.Vector3; v: THREE.Vector3 };
  colors(f: Friend): string[];
}

const GOLD = "hsl(48, 100%, 65%)";
const WHITE = "hsl(0, 0%, 100%)";
const rand = (a: number, b: number) => a + Math.random() * (b - a);
const around = (at: THREE.Vector3, dy = 0.8) => at.clone().add(new THREE.Vector3(0, dy, 0));
function radial(speed: number, up: number): THREE.Vector3 {
  const a = Math.random() * Math.PI * 2;
  const s = speed * rand(0.5, 1);
  return new THREE.Vector3(Math.cos(a) * s, rand(up * 0.5, up), Math.sin(a) * s * 0.6);
}

const RECIPES: Record<Effect | "puff", Recipe> = {
  sparkles: {
    shape: "star",
    count: 44,
    life: 1.6,
    size: 0.3,
    gravity: 3.2,
    sway: 0,
    spawn: (_, at) => ({ p: around(at), v: radial(2.4, 4.2) }),
    colors: (f) => [GOLD, WHITE, f.colors.body],
  },
  bolts: {
    shape: "star",
    count: 38,
    life: 0.9,
    size: 0.24,
    gravity: 1,
    sway: 0,
    spawn: (_, at) => ({ p: around(at), v: radial(4.5, 2) }),
    colors: (f) => [f.colors.accent, f.colors.body, WHITE],
  },
  fire: {
    shape: "dot",
    count: 60,
    life: 1,
    size: 0.42,
    gravity: -1.6,
    sway: 0.4,
    spawn: (_, at, mouth) => ({
      p: at.clone().add(mouth),
      v: new THREE.Vector3(rand(-0.5, 0.5), rand(0.4, 1.2), rand(2, 3.4)),
    }),
    colors: () => ["hsl(45, 100%, 62%)", "hsl(25, 100%, 56%)", "hsl(5, 90%, 55%)"],
  },
  snow: {
    shape: "flake",
    count: 80,
    life: 4,
    size: 0.24,
    gravity: 0,
    sway: 0.6,
    spawn: () => ({
      p: new THREE.Vector3(rand(-3.5, 3.5), rand(2.8, 4.4), rand(-1.5, 1.5)),
      v: new THREE.Vector3(0, rand(-1, -0.6), 0),
    }),
    colors: () => [WHITE, "hsl(200, 80%, 90%)"],
  },
  rings: {
    shape: "ring",
    count: 14,
    life: 2.2,
    size: 0.7,
    gravity: 0,
    sway: 0,
    spawn: (i, at) => ({ p: around(at, 0.1 + i * 0.12), v: new THREE.Vector3(0, 1.2, 0) }),
    colors: (f) => [f.colors.accent, f.colors.body],
  },
  hearts: {
    shape: "heart",
    count: 24,
    life: 2.4,
    size: 0.38,
    gravity: -0.4,
    sway: 0.5,
    spawn: (_, at) => ({ p: around(at, 1), v: radial(1.1, 1.6) }),
    colors: () => ["hsl(345, 95%, 66%)", "hsl(330, 90%, 78%)", "hsl(0, 90%, 70%)"],
  },
  bubbles: {
    shape: "ring",
    count: 32,
    life: 2.6,
    size: 0.32,
    gravity: -0.7,
    sway: 0.6,
    spawn: (_, at) => ({ p: around(at, 0.4), v: radial(0.9, 1.4) }),
    colors: () => ["hsl(195, 90%, 82%)", WHITE, "hsl(170, 80%, 80%)"],
  },
  rainbow: {
    shape: "star",
    count: 64,
    life: 1.9,
    size: 0.28,
    gravity: 2.6,
    sway: 0,
    spawn: (_, at) => ({ p: around(at, 1.2), v: radial(2.8, 4) }),
    colors: () => [0, 30, 55, 130, 200, 265, 300].map((h) => `hsl(${h}, 90%, 65%)`),
  },
  puff: {
    shape: "dot",
    count: 18,
    life: 0.6,
    size: 0.55,
    gravity: -0.5,
    sway: 0,
    spawn: (_, at) => ({ p: around(at, 0.25), v: radial(1.8, 0.8) }),
    colors: () => [WHITE, "hsl(0, 0%, 92%)"],
  },
};

// ---- Particle pictures, drawn once on a small canvas ---------------------------------------------

function draw(shape: Shape): THREE.CanvasTexture {
  const c = document.createElement("canvas");
  c.width = c.height = 64;
  const g = c.getContext("2d");
  if (g) {
    g.fillStyle = g.strokeStyle = WHITE;
    g.translate(32, 32);
    if (shape === "dot") {
      const r = g.createRadialGradient(0, 0, 0, 0, 0, 30);
      r.addColorStop(0, "hsl(0 0% 100% / 1)");
      r.addColorStop(1, "hsl(0 0% 100% / 0)");
      g.fillStyle = r;
      g.fillRect(-32, -32, 64, 64);
    } else if (shape === "star") {
      g.beginPath();
      for (let i = 0; i < 10; i++) {
        const r = i % 2 ? 11 : 28;
        const a = (i * Math.PI) / 5 - Math.PI / 2;
        g.lineTo(Math.cos(a) * r, Math.sin(a) * r);
      }
      g.fill();
    } else if (shape === "heart") {
      g.beginPath();
      g.moveTo(0, 22);
      g.bezierCurveTo(-34, 0, -20, -28, 0, -10);
      g.bezierCurveTo(20, -28, 34, 0, 0, 22);
      g.fill();
    } else if (shape === "ring") {
      g.lineWidth = 5;
      g.beginPath();
      g.arc(0, 0, 24, 0, Math.PI * 2);
      g.stroke();
      g.fillStyle = "hsl(0 0% 100% / 0.18)";
      g.fill();
    } else {
      g.lineWidth = 4;
      g.lineCap = "round";
      for (let i = 0; i < 6; i++) {
        g.rotate(Math.PI / 3);
        g.beginPath();
        g.moveTo(0, 0);
        g.lineTo(0, -26);
        g.moveTo(0, -16);
        g.lineTo(-7, -22);
        g.moveTo(0, -16);
        g.lineTo(7, -22);
        g.stroke();
      }
    }
  }
  const t = new THREE.CanvasTexture(c);
  t.colorSpace = THREE.SRGBColorSpace;
  return t;
}

interface Burst {
  points: THREE.Points;
  velocity: Float32Array;
  age: number;
  recipe: Recipe;
}

export class Effects {
  private readonly bursts: Burst[] = [];
  private readonly textures = new Map<Shape, THREE.CanvasTexture>();
  private readonly scene: THREE.Scene;

  constructor(scene: THREE.Scene) {
    this.scene = scene;
  }

  burst(
    kind: Effect | "puff",
    friend: Friend,
    at: THREE.Vector3,
    mouth: THREE.Vector3,
    calm: boolean,
  ) {
    const recipe = RECIPES[kind];
    const n = Math.max(4, Math.round(recipe.count * (calm ? 0.4 : 1)));
    const positions = new Float32Array(n * 3);
    const colors = new Float32Array(n * 3);
    const velocity = new Float32Array(n * 3);
    const palette = recipe.colors(friend).map((c) => new THREE.Color(c));
    for (let i = 0; i < n; i++) {
      const { p, v } = recipe.spawn(i, at, mouth);
      p.toArray(positions, i * 3);
      v.toArray(velocity, i * 3);
      (palette[i % palette.length] ?? palette[0])?.toArray(colors, i * 3);
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
    geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
    let texture = this.textures.get(recipe.shape);
    if (!texture) this.textures.set(recipe.shape, (texture = draw(recipe.shape)));
    const material = new THREE.PointsMaterial({
      size: recipe.size,
      map: texture,
      vertexColors: true,
      transparent: true,
      depthWrite: false,
    });
    const points = new THREE.Points(geometry, material);
    this.scene.add(points);
    this.bursts.push({ points, velocity, age: 0, recipe });
  }

  update(dt: number) {
    for (let b = this.bursts.length - 1; b >= 0; b--) {
      const burst = this.bursts[b];
      if (!burst) continue;
      const { points, velocity, recipe } = burst;
      burst.age += dt;
      const attr = points.geometry.getAttribute("position") as THREE.BufferAttribute;
      const p = attr.array as Float32Array;
      for (let i = 0; i < p.length; i += 3) {
        velocity[i + 1] = (velocity[i + 1] ?? 0) - recipe.gravity * dt;
        p[i] = (p[i] ?? 0) + ((velocity[i] ?? 0) + Math.sin(burst.age * 3 + i) * recipe.sway) * dt;
        p[i + 1] = (p[i + 1] ?? 0) + (velocity[i + 1] ?? 0) * dt;
        p[i + 2] = (p[i + 2] ?? 0) + (velocity[i + 2] ?? 0) * dt;
      }
      attr.needsUpdate = true;
      const k = burst.age / recipe.life;
      const material = points.material as THREE.PointsMaterial;
      material.opacity = k < 0.7 ? 1 : Math.max(0, 1 - (k - 0.7) / 0.3);
      if (recipe.shape === "ring") material.size = recipe.size * (1 + k * 0.8);
      if (k >= 1) {
        this.scene.remove(points);
        points.geometry.dispose();
        material.dispose();
        this.bursts.splice(b, 1);
      }
    }
  }

  dispose() {
    for (const b of this.bursts) {
      this.scene.remove(b.points);
      b.points.geometry.dispose();
      (b.points.material as THREE.Material).dispose();
    }
    this.bursts.length = 0;
    for (const t of this.textures.values()) t.dispose();
  }
}
