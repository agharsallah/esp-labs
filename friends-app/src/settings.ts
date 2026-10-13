// Per-device preferences, kept in this browser only (localStorage, which can be missing or
// blocked: the app then simply starts with the defaults every time).
import { useCallback, useEffect, useState } from "react";
import type { BoardMode } from "./board/useBoard.ts";

export type Theme = "auto" | "day" | "night";

export interface Settings {
  mode: BoardMode;
  sound: boolean;
  /** Fewer sparkles, gentler moves, no camera sway. Starts on when the system asks for less motion. */
  calm: boolean;
  theme: Theme;
}

const KEY = "card-friends:settings";

function defaults(): Settings {
  const reduce =
    typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches;
  return { mode: "live", sound: true, calm: reduce, theme: "auto" };
}

function load(): Settings {
  const base = defaults();
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return base;
    const saved = JSON.parse(raw) as Partial<Settings>;
    return {
      mode: saved.mode === "pretend" ? "pretend" : base.mode,
      sound: typeof saved.sound === "boolean" ? saved.sound : base.sound,
      calm: typeof saved.calm === "boolean" ? saved.calm : base.calm,
      theme: saved.theme === "day" || saved.theme === "night" ? saved.theme : base.theme,
    };
  } catch {
    return base;
  }
}

export function useSettings() {
  const [settings, setSettings] = useState<Settings>(load);
  useEffect(() => {
    try {
      localStorage.setItem(KEY, JSON.stringify(settings));
    } catch {
      // private window or blocked storage: keep the settings for this visit only
    }
    if (settings.theme === "auto") delete document.documentElement.dataset.theme;
    else document.documentElement.dataset.theme = settings.theme;
  }, [settings]);
  const change = useCallback(
    (patch: Partial<Settings>) => setSettings((s) => ({ ...s, ...patch })),
    [],
  );
  return [settings, change] as const;
}
