// A small picture of every friend, rendered once from the same 3D figures as the stage, for the
// friend shelf and the card tiles. One throwaway renderer; without WebGL the app falls back to a
// coloured circle with the friend's initial (Portrait.tsx).
import * as THREE from "three";
import { FRIENDS, type FriendId } from "../friends/characters.ts";
import { buildFigure } from "./figures.ts";

export type Portraits = Partial<Record<FriendId, string>>;

let made: Promise<Portraits> | null = null;

export function portraits(): Promise<Portraits> {
  made ??= new Promise((resolve) => {
    // After first paint: the shelf shows initials for a moment instead of blocking the page.
    requestAnimationFrame(() => resolve(render()));
  });
  return made;
}

function render(): Portraits {
  const out: Portraits = {};
  let renderer: THREE.WebGLRenderer | null = null;
  try {
    const canvas = document.createElement("canvas");
    renderer = new THREE.WebGLRenderer({
      canvas,
      antialias: true,
      alpha: true,
      preserveDrawingBuffer: true,
    });
    renderer.setSize(192, 192, false);
    const scene = new THREE.Scene();
    scene.add(new THREE.HemisphereLight("hsl(0, 0%, 100%)", "hsl(210, 80%, 85%)", 1.8));
    const sun = new THREE.DirectionalLight("hsl(0, 0%, 100%)", 1.8);
    sun.position.set(2, 4, 5);
    scene.add(sun);
    const camera = new THREE.PerspectiveCamera(30, 1, 0.1, 20);
    camera.position.set(0, 1.05, 3.6);
    camera.lookAt(0, 0.82, 0);
    for (const friend of FRIENDS) {
      const figure = buildFigure(friend);
      figure.root.rotation.y = -0.35;
      figure.update(0.5, 0);
      scene.add(figure.root);
      renderer.render(scene, camera);
      out[friend.id] = canvas.toDataURL("image/png");
      scene.remove(figure.root);
      figure.dispose();
    }
    console.info(`[portraits] ${FRIENDS.length} friends drawn`);
  } catch (e) {
    console.warn("[portraits] no WebGL here: the shelf shows initials", e);
  } finally {
    renderer?.dispose();
    renderer?.forceContextLoss();
  }
  return out;
}
