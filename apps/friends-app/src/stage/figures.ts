// The friends in 3D, built from a few spheres, cones and boxes with cartoon (toon) shading: no
// model files to load, and each friend is the same shape as on the board's screen
// (labs/rfid-lab-r8/src/characters.h). A figure stands on y = 0 and looks at +z (the camera).
import * as THREE from "three";
import { RoundedBoxGeometry } from "three/addons/geometries/RoundedBoxGeometry.js";
import type { Friend } from "../friends/characters.ts";

/** Shared colours of every face (design tokens of the 3D scene). */
const FACE = {
  eyeWhite: "hsl(0, 0%, 100%)",
  pupil: "hsl(232, 35%, 12%)",
  cheek: "hsl(350, 90%, 75%)",
  screen: "hsl(222, 35%, 14%)",
  glow: "hsl(178, 100%, 62%)",
};

export interface Figure {
  root: THREE.Group; // move, turn and squash this
  /** Idle life: blinking, breathing, a tail or wings. `excite` 0-1 speeds it up during a move. */
  update(t: number, excite: number): void;
  /** Where the friend's mouth is (fire, hearts), in the figure's own space. */
  mouth: THREE.Vector3;
  dispose(): void;
}

// ---- Materials and parts ------------------------------------------------------------------------

let toonRamp: THREE.DataTexture | null = null;
function ramp(): THREE.DataTexture {
  if (toonRamp) return toonRamp;
  // Three tones: shadow, mid, light. Nearest filtering gives the hard cartoon bands.
  toonRamp = new THREE.DataTexture(new Uint8Array([110, 190, 255]), 3, 1, THREE.RedFormat);
  toonRamp.minFilter = THREE.NearestFilter;
  toonRamp.magFilter = THREE.NearestFilter;
  toonRamp.needsUpdate = true;
  return toonRamp;
}

const UNIT = {
  sphere: new THREE.SphereGeometry(1, 32, 24),
  cone: new THREE.ConeGeometry(1, 1, 24),
  cylinder: new THREE.CylinderGeometry(1, 1, 1, 16),
  smile: new THREE.TorusGeometry(1, 0.22, 8, 20, Math.PI),
};

type V3 = [number, number, number];

class Kit {
  readonly materials: THREE.Material[] = [];
  readonly geometries: THREE.BufferGeometry[] = [];

  toon(color: string): THREE.MeshToonMaterial {
    const m = new THREE.MeshToonMaterial({ color: new THREE.Color(color), gradientMap: ramp() });
    this.materials.push(m);
    return m;
  }

  flat(color: string): THREE.MeshBasicMaterial {
    const m = new THREE.MeshBasicMaterial({ color: new THREE.Color(color) });
    this.materials.push(m);
    return m;
  }

  mesh(
    parent: THREE.Object3D,
    geometry: THREE.BufferGeometry,
    material: THREE.Material,
    at: V3,
    scale: V3 | number = 1,
    rotation: V3 = [0, 0, 0],
  ): THREE.Mesh {
    const m = new THREE.Mesh(geometry, material);
    m.position.set(...at);
    if (typeof scale === "number") m.scale.setScalar(scale);
    else m.scale.set(...scale);
    m.rotation.set(...rotation);
    parent.add(m);
    return m;
  }

  /** An empty group at `at`, to turn a part around its joint. */
  joint(parent: THREE.Object3D, at: V3): THREE.Group {
    const g = new THREE.Group();
    g.position.set(...at);
    parent.add(g);
    return g;
  }

  own<G extends THREE.BufferGeometry>(g: G): G {
    this.geometries.push(g);
    return g;
  }
}

/** Two eyes (or one) that can blink: a white, a pupil and a shine each. */
function eyes(kit: Kit, parent: THREE.Object3D, at: V3[], size: number): THREE.Group[] {
  const white = kit.toon(FACE.eyeWhite);
  const pupil = kit.flat(FACE.pupil);
  const shine = kit.flat(FACE.eyeWhite);
  return at.map((p) => {
    const eye = kit.joint(parent, p);
    kit.mesh(eye, UNIT.sphere, white, [0, 0, 0], [size, size, size * 0.6]);
    kit.mesh(eye, UNIT.sphere, pupil, [0, 0, size * 0.45], [size * 0.6, size * 0.6, size * 0.3]);
    kit.mesh(eye, UNIT.sphere, shine, [size * 0.22, size * 0.25, size * 0.62], size * 0.2);
    return eye;
  });
}

// ---- The cast -----------------------------------------------------------------------------------

export function buildFigure(friend: Friend): Figure {
  const kit = new Kit();
  const root = new THREE.Group();
  const body = kit.joint(root, [0, 0, 0]); // breathes
  const { colors } = friend;
  const skin = kit.toon(colors.body);
  const accent = kit.toon(colors.accent);
  const belly = kit.toon(colors.belly);
  const mouth = new THREE.Vector3(0, 0.74, 0.6);
  let eyeList: THREE.Group[] = [];
  let life: (t: number, excite: number) => void = () => {};

  const blob = (scale: V3 = [0.62, 0.66, 0.58], y = 0.72) =>
    kit.mesh(body, UNIT.sphere, skin, [0, y, 0], scale);
  const tummy = (scale: V3 = [0.42, 0.42, 0.3], at: V3 = [0, 0.6, 0.32]) =>
    kit.mesh(body, UNIT.sphere, belly, at, scale);
  const feet = (m: THREE.Material = skin) => {
    for (const side of [-1, 1])
      kit.mesh(body, UNIT.sphere, m, [side * 0.24, 0.07, 0.14], [0.17, 0.09, 0.22]);
  };
  const cheeks = () => {
    const pink = kit.toon(FACE.cheek);
    for (const side of [-1, 1])
      kit.mesh(body, UNIT.sphere, pink, [side * 0.37, 0.72, 0.43], [0.08, 0.06, 0.04]);
  };
  const smile = (at: V3 = [0, 0.73, 0.57], size = 0.07) =>
    kit.mesh(body, UNIT.smile, kit.flat(FACE.pupil), at, size, [0, 0, Math.PI]);
  const twoEyes = (y = 0.9, z = 0.5, gap = 0.2, size = 0.12) =>
    eyes(
      kit,
      body,
      [
        [-gap, y, z],
        [gap, y, z],
      ],
      size,
    );
  const pointyEars = (inner: THREE.Material, height = 0.34) => {
    for (const side of [-1, 1]) {
      const ear = kit.joint(body, [side * 0.3, 1.24, 0]);
      ear.rotation.z = -side * 0.35;
      kit.mesh(ear, UNIT.cone, skin, [0, height / 2, 0], [0.17, height, 0.12]);
      kit.mesh(ear, UNIT.cone, inner, [0, height * 0.55, 0.05], [0.09, height * 0.6, 0.05]);
    }
  };

  switch (friend.id) {
    case "fox": {
      blob();
      tummy();
      feet();
      pointyEars(accent);
      kit.mesh(body, UNIT.sphere, belly, [0, 0.74, 0.5], [0.2, 0.14, 0.2]); // snout
      kit.mesh(body, UNIT.sphere, accent, [0, 0.8, 0.69], 0.055); // nose
      eyeList = twoEyes(0.92, 0.47, 0.22);
      cheeks();
      const tail = kit.joint(body, [0, 0.35, -0.45]);
      kit.mesh(tail, UNIT.sphere, skin, [0, 0.18, -0.3], [0.2, 0.2, 0.42], [0.7, 0, 0]);
      kit.mesh(tail, UNIT.sphere, belly, [0, 0.42, -0.55], [0.13, 0.13, 0.18], [0.7, 0, 0]);
      life = (t, ex) => {
        tail.rotation.y = Math.sin(t * (4 + ex * 10)) * (0.4 + ex * 0.3);
      };
      mouth.set(0, 0.74, 0.7);
      break;
    }
    case "robot": {
      kit.mesh(body, kit.own(new RoundedBoxGeometry(1.1, 1.12, 0.95, 4, 0.22)), skin, [0, 0.74, 0]);
      kit.mesh(
        body,
        kit.own(new RoundedBoxGeometry(0.82, 0.48, 0.1, 3, 0.05)),
        kit.flat(FACE.screen),
        [0, 0.9, 0.47],
      );
      const glow = kit.flat(FACE.glow);
      eyeList = [-0.18, 0.18].map((x) => {
        const eye = kit.joint(body, [x, 0.94, 0.53]);
        kit.mesh(eye, kit.own(new RoundedBoxGeometry(0.15, 0.15, 0.04, 2, 0.03)), glow, [0, 0, 0]);
        return eye;
      });
      kit.mesh(body, kit.own(new THREE.BoxGeometry(0.26, 0.04, 0.03)), glow, [0, 0.77, 0.53]);
      for (const x of [-0.15, 0, 0.15]) kit.mesh(body, UNIT.sphere, accent, [x, 0.42, 0.47], 0.05);
      kit.mesh(body, UNIT.cylinder, accent, [0, 1.45, 0], [0.025, 0.3, 0.025]);
      const bulbMaterial = kit.flat(colors.accent);
      kit.mesh(body, UNIT.sphere, bulbMaterial, [0, 1.62, 0], 0.08);
      const arms = [-1, 1].map((side) => {
        const arm = kit.joint(body, [side * 0.6, 0.86, 0]);
        kit.mesh(arm, UNIT.cylinder, skin, [0, -0.2, 0], [0.06, 0.4, 0.06]);
        kit.mesh(arm, UNIT.sphere, accent, [0, -0.43, 0], 0.1);
        return arm;
      });
      feet(accent);
      life = (t, ex) => {
        arms.forEach((arm, i) => {
          arm.rotation.x = Math.sin(t * (2 + ex * 8) + i * Math.PI) * (0.15 + ex * 0.9);
          arm.rotation.z = (i ? 1 : -1) * (0.1 + ex * 0.5);
        });
        bulbMaterial.color.set(Math.sin(t * 6) > 0 ? colors.accent : FACE.glow);
      };
      mouth.set(0, 0.78, 0.6);
      break;
    }
    case "dragon": {
      blob();
      tummy();
      feet();
      for (const side of [-1, 1])
        kit.mesh(
          body,
          UNIT.cone,
          belly,
          [side * 0.2, 1.38, -0.05],
          [0.07, 0.24, 0.07],
          [0, 0, -side * 0.25],
        );
      const wingShape = new THREE.Shape();
      wingShape.moveTo(0, 0);
      wingShape.lineTo(0.75, 0.55);
      wingShape.lineTo(0.62, 0.05);
      wingShape.lineTo(0.8, -0.25);
      wingShape.closePath();
      const wingGeometry = kit.own(new THREE.ShapeGeometry(wingShape));
      const wingMaterial = kit.toon(colors.accent);
      wingMaterial.side = THREE.DoubleSide;
      const wings = [-1, 1].map((side) => {
        const wing = kit.joint(body, [side * 0.35, 0.95, -0.35]);
        kit.mesh(wing, wingGeometry, wingMaterial, [0, 0, 0], [side, 1, 1], [0, side * 0.5, 0]);
        return wing;
      });
      for (let i = 0; i < 3; i++)
        kit.mesh(
          body,
          UNIT.cone,
          accent,
          [0, 1.25 - i * 0.28, -0.42 - i * 0.12],
          [0.08, 0.2, 0.08],
          [-0.6 - i * 0.3, 0, 0],
        );
      kit.mesh(body, UNIT.cone, skin, [0, 0.3, -0.7], [0.14, 0.5, 0.14], [-1.3, 0, 0]);
      eyeList = twoEyes();
      cheeks();
      smile();
      life = (t, ex) => {
        wings.forEach((wing, i) => {
          wing.rotation.y = (i ? -1 : 1) * Math.sin(t * (3 + ex * 14)) * (0.25 + ex * 0.45);
        });
      };
      break;
    }
    case "penguin": {
      blob([0.6, 0.72, 0.56], 0.76);
      tummy([0.48, 0.56, 0.36], [0, 0.7, 0.26]);
      feet(accent);
      kit.mesh(body, UNIT.cone, accent, [0, 0.8, 0.62], [0.08, 0.18, 0.06], [Math.PI / 2, 0, 0]);
      eyeList = twoEyes(0.98, 0.48, 0.18, 0.1);
      cheeks();
      const flippers = [-1, 1].map((side) => {
        const f = kit.joint(body, [side * 0.56, 0.86, 0]);
        kit.mesh(f, UNIT.sphere, skin, [side * 0.04, -0.2, 0], [0.08, 0.3, 0.17]);
        return f;
      });
      life = (t, ex) => {
        flippers.forEach((f, i) => {
          f.rotation.z =
            (i ? 1 : -1) * (0.15 + Math.abs(Math.sin(t * (2 + ex * 12))) * (0.1 + ex * 0.8));
        });
      };
      mouth.set(0, 0.8, 0.72);
      break;
    }
    case "alien": {
      blob([0.6, 0.62, 0.56], 0.7);
      tummy();
      feet();
      eyeList = eyes(kit, body, [[0, 0.9, 0.44]], 0.22);
      smile([0, 0.6, 0.53], 0.08);
      const antennae = [-1, 1].map((side) => {
        const a = kit.joint(body, [side * 0.16, 1.24, 0]);
        a.rotation.z = -side * 0.4;
        kit.mesh(a, UNIT.cylinder, skin, [0, 0.18, 0], [0.025, 0.36, 0.025]);
        kit.mesh(a, UNIT.sphere, accent, [0, 0.4, 0], 0.08);
        return a;
      });
      kit.mesh(
        body,
        kit.own(new THREE.TorusGeometry(0.72, 0.05, 10, 48)),
        accent,
        [0, 0.52, 0],
        1,
        [Math.PI / 2, 0, 0],
      );
      life = (t, ex) => {
        antennae.forEach((a, i) => {
          a.rotation.x = Math.sin(t * (3 + ex * 8) + i) * (0.2 + ex * 0.4);
        });
      };
      break;
    }
    case "cat": {
      blob();
      tummy();
      feet();
      pointyEars(belly, 0.28);
      kit.mesh(body, UNIT.sphere, accent, [0, 0.79, 0.57], [0.05, 0.035, 0.04]);
      const whisker = kit.flat(colors.accent);
      for (const side of [-1, 1])
        for (const k of [-1, 0, 1])
          kit.mesh(
            body,
            UNIT.cylinder,
            whisker,
            [side * 0.36, 0.76 + k * 0.04, 0.5],
            [0.006, 0.26, 0.006],
            [0, 0, Math.PI / 2 + side * k * 0.18],
          );
      eyeList = twoEyes(0.92, 0.48, 0.21);
      cheeks();
      smile([0, 0.72, 0.57], 0.05);
      const tail = kit.joint(body, [0, 0.3, -0.48]);
      const curve = new THREE.CatmullRomCurve3([
        new THREE.Vector3(0, 0, 0),
        new THREE.Vector3(0.05, 0.25, -0.2),
        new THREE.Vector3(0.25, 0.6, -0.2),
        new THREE.Vector3(0.35, 0.8, 0),
      ]);
      kit.mesh(tail, kit.own(new THREE.TubeGeometry(curve, 24, 0.06, 8, false)), skin, [0, 0, 0]);
      life = (t, ex) => {
        tail.rotation.z = Math.sin(t * (1.5 + ex * 6)) * (0.35 + ex * 0.3);
      };
      break;
    }
    case "frog": {
      blob([0.7, 0.5, 0.6], 0.52);
      tummy([0.5, 0.32, 0.36], [0, 0.45, 0.3]);
      for (const side of [-1, 1]) {
        kit.mesh(body, UNIT.sphere, skin, [side * 0.28, 0.95, 0.12], 0.2);
        kit.mesh(body, UNIT.sphere, skin, [side * 0.52, 0.14, -0.08], [0.24, 0.12, 0.34]);
      }
      eyeList = eyes(
        kit,
        body,
        [
          [-0.28, 0.98, 0.27],
          [0.28, 0.98, 0.27],
        ],
        0.12,
      );
      kit.mesh(
        body,
        UNIT.smile,
        kit.flat(FACE.pupil),
        [0, 0.62, 0.55],
        [0.22, 0.12, 0.1],
        [0, 0, Math.PI],
      );
      for (const side of [-1, 1])
        kit.mesh(body, UNIT.sphere, accent, [side * 0.46, 0.6, 0.4], [0.08, 0.06, 0.04]);
      mouth.set(0, 0.6, 0.62);
      break;
    }
    case "unicorn": {
      blob();
      tummy();
      feet(belly);
      pointyEars(belly, 0.22);
      kit.mesh(body, UNIT.cone, accent, [0, 1.5, 0.18], [0.07, 0.42, 0.07], [0.35, 0, 0]);
      const mane: THREE.Mesh[] = [];
      for (let i = 0; i < 6; i++) {
        const hue = (i * 55 + 280) % 360;
        mane.push(
          kit.mesh(
            body,
            UNIT.sphere,
            kit.toon(`hsl(${hue}, 85%, 72%)`),
            [0, 1.32 - i * 0.14, -0.15 - i * 0.08],
            0.12,
          ),
        );
      }
      const tail = kit.joint(body, [0, 0.45, -0.55]);
      for (let i = 0; i < 4; i++)
        kit.mesh(
          tail,
          UNIT.sphere,
          kit.toon(`hsl(${(i * 70 + 300) % 360}, 85%, 72%)`),
          [0, -i * 0.1, -i * 0.1],
          0.12 - i * 0.015,
        );
      eyeList = twoEyes();
      cheeks();
      smile();
      life = (t, ex) => {
        mane.forEach((m, i) => {
          m.position.x = Math.sin(t * (2 + ex * 6) + i * 0.7) * (0.03 + ex * 0.04);
        });
        tail.rotation.y = Math.sin(t * (2 + ex * 8)) * (0.4 + ex * 0.3);
      };
      break;
    }
  }

  let nextBlink = 1 + Math.random() * 3;
  return {
    root,
    mouth,
    update(t, excite) {
      // Breathe, blink every few seconds (a quick close and open), then the friend's own life.
      const breathe = 1 + Math.sin(t * 2.2) * 0.02;
      body.scale.set(1 / breathe, breathe, 1 / breathe);
      if (t > nextBlink + 0.14) nextBlink = t + 2 + Math.random() * 3;
      const closed = t > nextBlink ? Math.max(0.08, Math.abs((t - nextBlink - 0.07) / 0.07)) : 1;
      for (const eye of eyeList) eye.scale.y = closed;
      life(t, excite);
    },
    dispose() {
      for (const m of kit.materials) m.dispose();
      for (const g of kit.geometries) g.dispose();
    },
  };
}
