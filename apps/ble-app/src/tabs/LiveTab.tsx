import { useState } from "react";

import { useNotify } from "../ble/useLink.ts";
import { type Sensors, decodeSensors } from "../protocol/sensors.ts";
import { LineChart } from "../ui/LineChart.tsx";
import { Needs } from "../ui/Needs.tsx";

export function LiveTab() {
  return (
    <Needs chr="sensors" lab="B5">
      <Live />
    </Needs>
  );
}

// Five notifications a second, 12 bytes each: the board pushes, the charts follow.
function Live() {
  const [samples, setSamples] = useState<(Sensors & { at: number })[]>([]);
  const [missed, setMissed] = useState(0);
  useNotify("sensors", (v) => {
    const s = decodeSensors(v);
    if (!s) return;
    setSamples((list) => {
      const prev = list[list.length - 1];
      if (prev && ((prev.seq + 1) & 255) !== s.seq) setMissed((m) => m + 1);
      return [...list, { ...s, at: performance.now() }].slice(-150);
    });
  });
  const pick = (k: keyof Sensors) => samples.map((s) => s[k]);
  const last = samples[samples.length - 1];
  return (
    <div className="grid two">
      <section className="card span intro">
        <p>
          The board notifies <b>Sensors</b> every 200 ms, 12 bytes: temperature, a touch reading on
          GPIO 1, free memory and the link's signal. Clip a jumper wire into GPIO 1 and touch its
          free end: the touch line jumps.
        </p>
        <p className="muted small">
          {samples.length} samples · sequence {last?.seq ?? "–"} · {missed} gaps
        </p>
      </section>
      <section className="card">
        <LineChart values={pick("tempC")} tone="sub" label="Temperature" unit=" °C" digits={2} />
      </section>
      <section className="card">
        <LineChart values={pick("touch")} tone="read" label="Touch (GPIO 1)" digits={0} />
      </section>
      <section className="card">
        <LineChart
          values={samples.map((s) => s.heap / 1024)}
          tone="notify"
          label="Free heap"
          unit=" kB"
          digits={1}
        />
      </section>
      <section className="card">
        <LineChart
          values={pick("rssi")}
          tone="radio"
          label="Signal (RSSI)"
          unit=" dBm"
          digits={0}
        />
      </section>
    </div>
  );
}
