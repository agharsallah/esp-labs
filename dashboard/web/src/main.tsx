import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App.tsx";
import { SessionGate } from "./SessionGate.tsx";
import "./styles.css";

const root = document.getElementById("root");
if (!root) throw new Error("index.html is missing #root");
createRoot(root).render(
  <StrictMode>
    <SessionGate>{(signOut) => <App onSignOut={signOut} />}</SessionGate>
  </StrictMode>,
);
