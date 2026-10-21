import type { ReactNode } from "react";

// A small turning ring for buttons and labels while the board works (it stands still when the
// system asks for reduced motion; the label still says what is happening).
export function Spinner() {
  return <span className="spinner" aria-hidden="true" />;
}

// A button's label: what it does, or a spinner and what it is doing right now.
export function BusyText({
  busy,
  idle,
  active,
}: {
  busy: boolean;
  idle: ReactNode;
  active: string;
}) {
  return busy ? (
    <>
      <Spinner /> {active}
    </>
  ) : (
    <>{idle}</>
  );
}

// Grey placeholder rows while the first data arrives from the board.
export function Skeleton({ rows, label }: { rows: number; label: string }) {
  return (
    <div className="skeleton" role="status" aria-label={label}>
      {Array.from({ length: rows }, (_, i) => (
        <span key={i} />
      ))}
    </div>
  );
}
