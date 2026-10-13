// The 3D stage: a little island under a sky (the sky itself is CSS behind a transparent canvas,
// so it can fade between friends' colours for free), drifting clouds, twinkling stars, and one
// card per friend on stage with the friend standing on it.
//
// React tells the stage who is on it (setCast); the stage works out the rest: a new friend pops
// out of its card with a puff and plays its move with its sparkles, a friend tapped again plays
// it again, one that left hops back in. Poking a friend (click or touch) makes it play too.
import * as THREE from "three";
import { friendById, type Friend } from "../friends/characters.ts";
import { type Actor, spots } from "../play/cast.ts";
import { buildFigure, type Figure } from "./figures.ts";
import { Effects } from "./effects.ts";
import { DURATION, enter, leave, move, type Pose, REST } from "./moves.ts";

/** Colours of the stage itself (design tokens of the 3D scene). */
const STAGE = {
  card: "hsl(40, 30%, 97%)",
  cardInk: "hsl(232, 30%, 20%)",
  cloud: "hsl(0, 0%, 100%)",
  shadow: "hsl(240, 40%, 10%)",
  sky: "hsl(210, 80%, 85%)",
  island: "hsl(110, 40%, 55%)",
  bush: "hsl(130, 45%, 40%)",
};

const ENTER_S = 0.8;
const LEAVE_S = 0.6;
const FOV = 35;

type Phase = "enter" | "move" | "idle" | "leave";

interface Player {
  uid: string;
  friend: Friend;
  figure: Figure;
  holder: THREE.Group; // the card, the glow, the shadow and the friend
  glow: THREE.Mesh<THREE.RingGeometry, THREE.MeshBasicMaterial>;
  shadow: THREE.Mesh;
  disposables: { dispose(): void }[];
  x: number;
  targetX: number;
  phase: Phase;
  phaseT: number;
  cue: number;
}

export interface WorldOptions {
  calm: boolean;
  onPoke(uid: string): void;
}

export class StageWorld {
  private readonly renderer: THREE.WebGLRenderer;
  private readonly scene = new THREE.Scene();
  private readonly camera = new THREE.PerspectiveCamera(FOV, 1, 0.1, 100);
  private readonly effects: Effects;
  private readonly players = new Map<string, Player>();
  private readonly island: THREE.MeshToonMaterial;
  private readonly clouds: THREE.Group[] = [];
  private readonly stars: THREE.Points;
  private readonly raycaster = new THREE.Raycaster();
  private readonly pointer = new THREE.Vector2();
  private readonly cardGeometry: THREE.ExtrudeGeometry;
  private readonly resize: ResizeObserver;
  private readonly canvas: HTMLCanvasElement;
  private calm: boolean;
  private readonly onPoke: (uid: string) => void;
  private lead: Player | null = null; // the friend played last: the island takes its colour
  private distance = 7;
  private last = performance.now();
  private time = 0;

  constructor(canvas: HTMLCanvasElement, options: WorldOptions) {
    this.canvas = canvas;
    this.calm = options.calm;
    this.onPoke = options.onPoke;
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.setClearColor(STAGE.shadow, 0); // transparent: the CSS sky shows through
    this.effects = new Effects(this.scene);

    this.scene.add(new THREE.HemisphereLight(STAGE.cloud, STAGE.sky, 1.7));
    const sun = new THREE.DirectionalLight(STAGE.cloud, 1.8);
    sun.position.set(3, 6, 4);
    this.scene.add(sun);

    // The island the cards lie on, with a few round bushes on its rim.
    this.island = new THREE.MeshToonMaterial({ color: new THREE.Color(STAGE.island) });
    const ground = new THREE.Mesh(new THREE.CylinderGeometry(7, 7.6, 0.6, 64), this.island);
    ground.position.y = -0.33;
    this.scene.add(ground);
    const bush = new THREE.MeshToonMaterial({ color: new THREE.Color(STAGE.bush) });
    for (let i = 0; i < 9; i++) {
      const a = Math.PI * (0.95 + (i / 8) * 1.1);
      const b = new THREE.Mesh(new THREE.SphereGeometry(0.35 + (i % 3) * 0.12, 16, 12), bush);
      b.position.set(Math.cos(a) * 6.2, 0, Math.sin(a) * 4 - 1);
      this.scene.add(b);
    }

    // Clouds far behind, drifting slowly.
    const cloudMaterial = new THREE.MeshToonMaterial({ color: new THREE.Color(STAGE.cloud) });
    const puff = new THREE.SphereGeometry(1, 16, 12);
    for (let i = 0; i < 4; i++) {
      const cloud = new THREE.Group();
      for (let k = 0; k < 4; k++) {
        const m = new THREE.Mesh(puff, cloudMaterial);
        m.position.set(k * 0.7 - 1, Math.sin(k * 2) * 0.2, 0);
        m.scale.setScalar(0.55 + (k % 2) * 0.25);
        cloud.add(m);
      }
      cloud.position.set(-9 + i * 6, 3.4 + (i % 2) * 1.1, -9 - (i % 3));
      this.clouds.push(cloud);
      this.scene.add(cloud);
    }

    // Stars, for the darker skies.
    const starPositions = new Float32Array(150 * 3);
    for (let i = 0; i < 150; i++)
      starPositions.set(
        [(Math.random() - 0.5) * 40, 2 + Math.random() * 12, -16 - Math.random() * 6],
        i * 3,
      );
    const starGeometry = new THREE.BufferGeometry();
    starGeometry.setAttribute("position", new THREE.BufferAttribute(starPositions, 3));
    this.stars = new THREE.Points(
      starGeometry,
      new THREE.PointsMaterial({
        color: new THREE.Color(STAGE.cloud),
        size: 0.09,
        transparent: true,
      }),
    );
    this.scene.add(this.stars);

    // Every friend stands on a card: a rounded plate like the real ones.
    const shape = new THREE.Shape();
    const w = 1.3,
      h = 0.84,
      r = 0.1;
    shape.moveTo(-w / 2 + r, -h / 2);
    shape.lineTo(w / 2 - r, -h / 2);
    shape.quadraticCurveTo(w / 2, -h / 2, w / 2, -h / 2 + r);
    shape.lineTo(w / 2, h / 2 - r);
    shape.quadraticCurveTo(w / 2, h / 2, w / 2 - r, h / 2);
    shape.lineTo(-w / 2 + r, h / 2);
    shape.quadraticCurveTo(-w / 2, h / 2, -w / 2, h / 2 - r);
    shape.lineTo(-w / 2, -h / 2 + r);
    shape.quadraticCurveTo(-w / 2, -h / 2, -w / 2 + r, -h / 2);
    this.cardGeometry = new THREE.ExtrudeGeometry(shape, { depth: 0.05, bevelEnabled: false });
    this.cardGeometry.rotateX(-Math.PI / 2);

    canvas.addEventListener("pointermove", this.handleMove);
    canvas.addEventListener("pointerdown", this.handleDown);
    canvas.addEventListener("webglcontextlost", this.handleLost);
    this.resize = new ResizeObserver(() => this.fit());
    this.resize.observe(canvas);
    this.fit();
    this.renderer.setAnimationLoop(this.tick);
    console.info("[stage] ready");
  }

  setCalm(calm: boolean) {
    this.calm = calm;
  }

  /** Who is on stage, newest first. Places follow the card numbers, so nobody swaps places. */
  setCast(actors: Actor[]) {
    const ordered = [...actors].sort((a, b) => a.number - b.number);
    const xs = spots(ordered.length);
    ordered.forEach((actor, i) => {
      const friend = friendById(actor.character);
      if (!friend) return;
      const x = xs[i] ?? 0;
      let player = this.players.get(actor.uid);
      if (player && (player.phase === "leave" || player.friend.id !== friend.id)) {
        this.remove(player); // a friend moved onto this card: the old one goes at once
        player = undefined;
      }
      if (!player) {
        player = this.create(actor, friend, x);
        this.players.set(actor.uid, player);
        this.burst(player, "puff");
        console.info(`[stage] ${friend.name} pops out of card ${actor.number}`);
      } else if (player.cue !== actor.cue) {
        player.cue = actor.cue;
        this.play(player);
      }
      player.targetX = x;
    });
    for (const p of this.players.values())
      if (!actors.some((a) => a.uid === p.uid) && p.phase !== "leave") {
        p.phase = "leave";
        p.phaseT = 0;
      }
  }

  /** Make a friend on stage play its move again (a poke). */
  poke(uid: string) {
    const p = this.players.get(uid);
    if (p && p.phase !== "leave" && p.phase !== "enter") this.play(p);
  }

  dispose() {
    this.renderer.setAnimationLoop(null);
    this.resize.disconnect();
    this.canvas.removeEventListener("pointermove", this.handleMove);
    this.canvas.removeEventListener("pointerdown", this.handleDown);
    this.canvas.removeEventListener("webglcontextlost", this.handleLost);
    for (const p of [...this.players.values()]) this.remove(p);
    this.effects.dispose();
    this.cardGeometry.dispose();
    this.scene.traverse((o) => {
      if (o instanceof THREE.Mesh || o instanceof THREE.Points) {
        o.geometry.dispose();
        const m = o.material as THREE.Material | THREE.Material[];
        for (const one of Array.isArray(m) ? m : [m]) one.dispose();
      }
    });
    this.renderer.dispose();
  }

  // ---- Players ---------------------------------------------------------------------------------

  private create(actor: Actor, friend: Friend, x: number): Player {
    const holder = new THREE.Group();
    holder.position.x = x;
    holder.userData.uid = actor.uid;
    const disposables: { dispose(): void }[] = [];

    const cardMaterial = new THREE.MeshToonMaterial({ color: new THREE.Color(STAGE.card) });
    const card = new THREE.Mesh(this.cardGeometry, cardMaterial);
    card.position.y = -0.05;
    holder.add(card);
    const label = labelTexture(actor.number, friend);
    const labelMaterial = new THREE.MeshBasicMaterial({ map: label, transparent: true });
    const labelPlane = new THREE.Mesh(new THREE.PlaneGeometry(1.24, 0.78), labelMaterial);
    labelPlane.rotation.x = -Math.PI / 2;
    labelPlane.position.y = 0.002;
    holder.add(labelPlane);
    disposables.push(cardMaterial, label, labelMaterial, labelPlane.geometry);

    const glow = new THREE.Mesh(
      new THREE.RingGeometry(0.78, 0.95, 48),
      new THREE.MeshBasicMaterial({
        color: new THREE.Color(friend.colors.accent),
        transparent: true,
        opacity: 0,
      }),
    );
    glow.rotation.x = -Math.PI / 2;
    glow.position.y = 0.01;
    holder.add(glow);
    disposables.push(glow.geometry, glow.material);

    const shadow = new THREE.Mesh(
      new THREE.CircleGeometry(0.5, 32),
      new THREE.MeshBasicMaterial({
        color: new THREE.Color(STAGE.shadow),
        transparent: true,
        opacity: 0.22,
        depthWrite: false,
      }),
    );
    shadow.rotation.x = -Math.PI / 2;
    shadow.position.y = 0.006;
    holder.add(shadow);
    disposables.push(shadow.geometry, shadow.material as THREE.Material);

    const figure = buildFigure(friend);
    holder.add(figure.root);
    disposables.push(figure);
    this.scene.add(holder);
    return {
      uid: actor.uid,
      friend,
      figure,
      holder,
      glow,
      shadow,
      disposables,
      x,
      targetX: x,
      phase: "enter",
      phaseT: 0,
      cue: actor.cue,
    };
  }

  private remove(p: Player) {
    this.scene.remove(p.holder);
    for (const d of p.disposables) d.dispose();
    if (this.lead === p) this.lead = null;
    this.players.delete(p.uid);
  }

  private play(p: Player) {
    this.lead = p;
    p.phase = "move";
    p.phaseT = 0;
    p.glow.material.opacity = 1;
    this.burst(p, p.friend.effect);
  }

  private burst(p: Player, kind: Parameters<Effects["burst"]>[0]) {
    this.effects.burst(kind, p.friend, p.holder.position.clone(), p.figure.mouth, this.calm);
  }

  // ---- Every frame --------------------------------------------------------------------------------

  private readonly tick = () => {
    const now = performance.now();
    const dt = Math.min(0.05, (now - this.last) / 1000);
    this.last = now;
    this.time += dt;
    const t = this.time;

    for (const p of [...this.players.values()]) {
      p.phaseT += dt;
      let pose: Pose = REST;
      if (p.phase === "enter") {
        pose = enter(p.phaseT / ENTER_S);
        if (p.phaseT >= ENTER_S) this.play(p);
      } else if (p.phase === "move") {
        const d = DURATION[p.friend.move];
        pose = move(p.friend.move, Math.min(1, p.phaseT / d), this.calm);
        if (p.phaseT >= d) p.phase = "idle";
      } else if (p.phase === "leave") {
        pose = leave(p.phaseT / LEAVE_S);
        if (p.phaseT >= LEAVE_S) {
          this.remove(p);
          continue;
        }
      } else {
        // Idle: look a little towards the pointer, sway gently.
        pose = {
          ...REST,
          turn: this.calm ? 0 : this.pointer.x * 0.35,
          tiltZ: Math.sin(t * 1.3 + p.x) * 0.03,
        };
      }
      p.x += (p.targetX - p.x) * Math.min(1, dt * 4);
      p.holder.position.x = p.x;
      const root = p.figure.root;
      root.position.set(pose.x, Math.max(-1, pose.y), pose.z);
      root.rotation.set(pose.tiltX, pose.turn, pose.tiltZ, "YXZ");
      root.scale.set(pose.scale * pose.squash, pose.scale / pose.squash, pose.scale * pose.squash);
      p.figure.update(t, pose.excite);
      // The shadow shrinks as the friend goes up; the glow fades after a show.
      const lift = Math.max(0, pose.y);
      p.shadow.scale.setScalar(Math.max(0.3, 1 - lift * 0.3) * pose.scale);
      p.shadow.position.x = pose.x;
      p.shadow.position.z = pose.z;
      p.glow.material.opacity = Math.max(0, p.glow.material.opacity - dt * 0.6);
      p.glow.scale.setScalar(1 + Math.sin(t * 6) * 0.04);
    }

    // The island takes the ground colour of the friend who played last, slowly.
    if (this.lead)
      this.island.color.lerp(
        new THREE.Color(this.lead.friend.colors.ground),
        Math.min(1, dt * 1.5),
      );
    for (const [i, c] of this.clouds.entries()) {
      c.position.x += dt * (0.25 + i * 0.05) * (this.calm ? 0.4 : 1);
      if (c.position.x > 13) c.position.x = -13;
    }
    (this.stars.material as THREE.PointsMaterial).opacity = 0.55 + Math.sin(t * 1.7) * 0.25;
    this.effects.update(dt);

    // Camera: back off to fit everyone; a slight sway with the pointer unless calm.
    const target = this.fitDistance();
    this.distance += (target - this.distance) * Math.min(1, dt * 2);
    const sway = this.calm ? 0 : 1;
    this.camera.position.set(
      this.pointer.x * 0.5 * sway,
      2.3 + this.pointer.y * 0.25 * sway,
      this.distance,
    );
    this.camera.lookAt(0, 0.85, 0);
    this.renderer.render(this.scene, this.camera);
  };

  /** How far back the camera stands so 1, 2 or 3 friends fit, also on a narrow phone. */
  private fitDistance(): number {
    const count = [...this.players.values()].filter((p) => p.phase !== "leave").length;
    const halfWidth = count <= 1 ? 1.5 : count === 2 ? 2.3 : 3.1;
    const v = THREE.MathUtils.degToRad(FOV) / 2;
    const h = Math.atan(Math.tan(v) * this.camera.aspect);
    return Math.max(5.2, halfWidth / Math.tan(h) + 1, 1.7 / Math.tan(v));
  }

  private fit() {
    const w = this.canvas.clientWidth || 1;
    const h = this.canvas.clientHeight || 1;
    this.renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
  }

  private readonly handleMove = (e: PointerEvent) => {
    const r = this.canvas.getBoundingClientRect();
    this.pointer.set(
      ((e.clientX - r.left) / r.width) * 2 - 1,
      -(((e.clientY - r.top) / r.height) * 2 - 1),
    );
  };

  private readonly handleDown = (e: PointerEvent) => {
    this.handleMove(e);
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const roots = [...this.players.values()].map((p) => p.holder);
    const hit = this.raycaster.intersectObjects(roots, true)[0];
    let o: THREE.Object3D | null = hit?.object ?? null;
    while (o && !o.userData.uid) o = o.parent;
    const uid = o?.userData.uid as string | undefined;
    if (!uid) return;
    this.poke(uid);
    this.onPoke(uid);
  };

  private readonly handleLost = (e: Event) => {
    e.preventDefault();
    console.warn("[stage] the 3D context was lost (the browser needed the GPU back)");
  };
}

/** The top of a card: a stripe in the friend's colour, its number and the contactless waves. */
function labelTexture(number: number, friend: Friend): THREE.CanvasTexture {
  const c = document.createElement("canvas");
  c.width = 256;
  c.height = 160;
  const g = c.getContext("2d");
  if (g) {
    g.fillStyle = friend.colors.body;
    g.fillRect(0, 0, 256, 34);
    g.fillStyle = STAGE.cardInk;
    g.font = "700 72px Fredoka, system-ui, sans-serif";
    g.textBaseline = "middle";
    g.fillText(String(number), 22, 100);
    g.strokeStyle = friend.colors.accent;
    g.lineWidth = 7;
    g.lineCap = "round";
    for (let i = 0; i < 3; i++) {
      g.beginPath();
      g.arc(170, 100, 18 + i * 18, -0.7, 0.7);
      g.stroke();
    }
  }
  const t = new THREE.CanvasTexture(c);
  t.colorSpace = THREE.SRGBColorSpace;
  return t;
}
