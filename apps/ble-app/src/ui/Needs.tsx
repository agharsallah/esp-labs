import type { ReactNode } from "react";

import type { ChrKey } from "../ble/uuids.ts";
import { useLink } from "../ble/useLink.ts";

// Shows its children only when the connected board has the characteristic a tab needs.
export function Needs({ chr, lab, children }: { chr: ChrKey; lab: string; children: ReactNode }) {
  const l = useLink();
  if (l.status !== "connected")
    return (
      <div className="needs">
        <p>Not connected.</p>
        <p className="muted">Go to Connect and pick a bench board. This tab works with {lab}.</p>
      </div>
    );
  if (!l.has(chr))
    return (
      <div className="needs">
        <p>{l.deviceName} doesn't have this.</p>
        <p className="muted">This tab needs {lab}. Flash that lab and connect again.</p>
      </div>
    );
  return <>{children}</>;
}
