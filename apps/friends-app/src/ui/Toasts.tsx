import { useCallback, useState } from "react";

export interface Toast {
  id: number;
  text: string;
  ok: boolean;
}

export function useToasts() {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const notify = useCallback((text: string, ok: boolean) => {
    const id = Date.now() + Math.random();
    setToasts((t) => [...t, { id, text, ok }].slice(-3));
    setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), ok ? 3200 : 7000);
  }, []);
  return { toasts, notify };
}

export function Toasts({ toasts }: { toasts: Toast[] }) {
  return (
    <div className="toasts" role="status">
      {toasts.map((t) => (
        <p key={t.id} className={t.ok ? "ok" : "bad"}>
          {t.text}
        </p>
      ))}
    </div>
  );
}
