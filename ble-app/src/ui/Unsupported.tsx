// Web Bluetooth is a Chromium feature, and only on secure pages. Say exactly what to do instead.
export function Unsupported() {
  const secure = typeof window !== "undefined" && window.isSecureContext;
  return (
    <main className="unsupported">
      <div className="card">
        <p className="eyebrow">Web Bluetooth isn't available here</p>
        <h1>This browser can't talk to the board.</h1>
        {!secure ? (
          <p>
            The page isn't secure. Web Bluetooth only works on <code>https://</code> or{" "}
            <code>http://localhost</code>. On the Mac open <code>http://localhost:5177</code>; for a
            phone run <code>pnpm tunnel</code> and open the <code>https://…trycloudflare.com</code>{" "}
            address.
          </p>
        ) : (
          <p>Safari and Firefox don't implement Web Bluetooth. Use one of these:</p>
        )}
        <ul className="platforms">
          <li>
            <b>Mac</b> Chrome or Edge, at <code>http://localhost:5177</code>
          </li>
          <li>
            <b>Android</b> Chrome, at the tunnel's https address (Bluetooth and Location on)
          </li>
          <li>
            <b>iPhone / iPad</b> the free <b>Bluefy</b> browser from the App Store, at the tunnel's
            https address
          </li>
        </ul>
        <p className="muted">
          No browser at hand? nRF Connect or LightBlue reach the same characteristics by hand (lab
          B3).
        </p>
      </div>
    </main>
  );
}
