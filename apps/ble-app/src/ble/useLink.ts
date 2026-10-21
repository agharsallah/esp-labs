import { useEffect, useSyncExternalStore } from "react";

import type { ChrKey } from "./uuids.ts";
import { link } from "./link.ts";

/** Re-renders whenever the link changes (status, log, services). */
export function useLink() {
  useSyncExternalStore(link.subscribe, link.snapshot);
  return link;
}

/** Subscribe to a characteristic while connected; unsubscribes when the component goes away. */
export function useNotify(key: ChrKey, handler: (v: DataView) => void, enabled = true) {
  const l = useLink();
  const ready = l.status === "connected" && l.has(key) && enabled;
  useEffect(() => {
    if (!ready) return;
    let stop: (() => void) | null = null;
    let gone = false;
    link.listen(key, handler).then((s) => (gone ? s() : (stop = s)));
    return () => {
      gone = true;
      stop?.();
    };
    // The handler is read through a ref-like closure each time; resubscribing on every render would spam the CCCD.
  }, [ready, key]);
}
