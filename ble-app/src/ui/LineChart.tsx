// A small SVG line chart that scales to its box. Values oldest first.
type Props = {
  values: number[];
  tone: string;
  label: string;
  unit?: string;
  height?: number;
  min?: number;
  max?: number;
  digits?: number;
};

export function LineChart({
  values,
  tone,
  label,
  unit = "",
  height = 96,
  min,
  max,
  digits = 1,
}: Props) {
  const w = 300;
  const lo = min ?? Math.min(...values, Number.POSITIVE_INFINITY);
  const hiRaw = max ?? Math.max(...values, Number.NEGATIVE_INFINITY);
  const hi = hiRaw - lo < 1e-6 ? lo + 1 : hiRaw;
  const pts = values.map(
    (v, i) =>
      `${(i / Math.max(values.length - 1, 1)) * w},${height - 4 - ((v - lo) / (hi - lo)) * (height - 8)}`,
  );
  const last = values[values.length - 1];
  return (
    <figure className="chart">
      <figcaption>
        <span>{label}</span>
        <b style={{ color: `var(--${tone})` }}>
          {last === undefined ? "–" : `${last.toFixed(digits)}${unit}`}
        </b>
      </figcaption>
      <svg
        viewBox={`0 0 ${w} ${height}`}
        preserveAspectRatio="none"
        role="img"
        aria-label={`${label}: ${values.length} samples, latest ${last ?? "none"}${unit}`}
      >
        {[0.25, 0.5, 0.75].map((k) => (
          <line key={k} x1="0" x2={w} y1={height * k} y2={height * k} className="grid" />
        ))}
        {pts.length > 1 && (
          <polyline
            points={pts.join(" ")}
            fill="none"
            stroke={`var(--${tone})`}
            strokeWidth="2"
            vectorEffect="non-scaling-stroke"
          />
        )}
      </svg>
      {values.length > 0 && (
        <div className="chart-range">
          <span>
            {lo.toFixed(digits)}
            {unit}
          </span>
          <span>
            {hi.toFixed(digits)}
            {unit}
          </span>
        </div>
      )}
    </figure>
  );
}
