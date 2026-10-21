// The phone page S4 serves at http://bench.local/: one file, no outside scripts or fonts, so it
// works on a network without internet. It asks for the admin token first and holds no audio
// until then; with it, a WebSocket brings the live level and state ~10 times a second, and each
// clip is fetched as a WAV, decoded by the browser (Web Audio) and drawn as a waveform you can
// play, scrub, download or delete, or play on the board's own speaker (with a speed choice and
// the board's progress, live from the WebSocket).
//
// Colours are design tokens in the :root block, with a dark set for phones in dark mode.
#pragma once

#include <pgmspace.h>

static const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Bench recorder</title>
<style>
:root {
  --bg: #f6f4f1; --surface: #ffffff; --surface-2: #efebe7; --line: #d9d1ca;
  --text: #1d1b20; --muted: #585260; --accent: #a3236b; --on-accent: #ffffff;
  --rec: #b3261e; --ok: #1d6b50; --warn: #855700; --focus: #1a56db;
  --wave: #c9b9c3; --wave-played: #a3236b; --playhead: #1d1b20; --meter-off: #e4ddd7;
  --radius: 14px; --gap: 16px;
  font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0f1217; --surface: #171c23; --surface-2: #202731; --line: #2d3541;
    --text: #eef1f5; --muted: #a7b1be; --accent: #ff8fcf; --on-accent: #1a0f16;
    --rec: #ff6b63; --ok: #5fdcb4; --warn: #ffc061; --focus: #8ab4ff;
    --wave: #4a3f49; --wave-played: #ff8fcf; --playhead: #eef1f5; --meter-off: #252c36;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); line-height: 1.45; }
main { max-width: 640px; margin: 0 auto; padding: var(--gap); display: grid; gap: var(--gap); }
header { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; flex-wrap: wrap; }
h1 { font-size: 1.35rem; margin: 0; letter-spacing: -0.01em; }
h1 small { color: var(--muted); font-weight: 500; font-size: 0.85rem; margin-left: 6px; }
h2 { font-size: 1rem; margin: 0 0 8px; }
.pill { font-size: 0.8rem; color: var(--muted); border: 1px solid var(--line); border-radius: 99px; padding: 2px 10px; }
.card { background: var(--surface); border: 1px solid var(--line); border-radius: var(--radius); padding: var(--gap); }
.muted { color: var(--muted); font-size: 0.9rem; }
label { display: block; font-weight: 600; margin-bottom: 6px; }
input { width: 100%; font: inherit; padding: 10px 12px; border-radius: 10px; border: 1px solid var(--line); background: var(--surface-2); color: var(--text); }
button, .button { font: inherit; font-weight: 600; border-radius: 10px; border: 1px solid var(--line); background: var(--surface-2);
  color: var(--text); padding: 10px 16px; min-height: 44px; cursor: pointer; text-decoration: none; display: inline-flex; align-items: center; gap: 6px; }
button.primary { background: var(--accent); color: var(--on-accent); border-color: var(--accent); }
button[aria-pressed="true"] { background: var(--ok); color: var(--surface); border-color: var(--ok); }
button:disabled { opacity: 0.45; cursor: default; }
:focus-visible { outline: 3px solid var(--focus); outline-offset: 2px; }
.row { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
form .row { margin-top: 10px; }
.deck { display: grid; gap: 14px; }
.state { display: flex; align-items: center; gap: 12px; }
.lamp { width: 18px; height: 18px; border-radius: 50%; background: var(--meter-off); flex: none; }
.state[data-state="armed"] .lamp { background: var(--ok); }
.state[data-state="recording"] .lamp { background: var(--rec); animation: pulse 1s ease-in-out infinite; }
.state strong { font-size: 1.6rem; letter-spacing: 0.04em; }
.state[data-state="recording"] strong { color: var(--rec); }
@keyframes pulse { 50% { opacity: 0.35; } }
@media (prefers-reduced-motion: reduce) { .state[data-state="recording"] .lamp { animation: none; } }
.meter { position: relative; height: 22px; display: grid; grid-template-columns: repeat(30, 1fr); gap: 3px; }
.meter i { border-radius: 3px; background: var(--meter-off); }
.meter i.on { background: var(--ok); }
.meter i.on.loud { background: var(--warn); }
.meter i.on.hot { background: var(--rec); }
.meter .trig { position: absolute; top: -5px; bottom: -5px; width: 2px; background: var(--text); }
.facts { display: flex; gap: 6px 18px; flex-wrap: wrap; font-size: 0.85rem; color: var(--muted); }
ol { list-style: none; margin: 0; padding: 0; display: grid; gap: 12px; }
.clip { display: grid; gap: 8px; }
.clip header { align-items: center; }
.clip h3 { margin: 0; font-size: 1rem; }
.clip canvas { width: 100%; height: 72px; display: block; border-radius: 10px; background: var(--surface-2); cursor: pointer; touch-action: manipulation; }
.empty { text-align: center; padding: 28px 8px; }
.board-progress { width: 100%; height: 6px; accent-color: var(--accent); }
select { font: inherit; padding: 8px 10px; border-radius: 10px; border: 1px solid var(--line); background: var(--surface-2); color: var(--text); min-height: 44px; }
.speed { display: flex; align-items: center; gap: 8px; margin-bottom: 12px; }
.speed label { margin: 0; font-weight: 500; }
[hidden] { display: none !important; }
</style>
</head>
<body>
<main>
  <header>
    <h1>Bench recorder <small id="mic">S4</small></h1>
    <span class="pill" id="link" role="status" aria-live="polite">locked</span>
  </header>

  <form class="card" id="unlock" autocomplete="off">
    <h2>Unlock</h2>
    <p class="muted">This page can hear your room, so it needs the bench's admin token. Type <code>token</code> in the serial monitor and paste it here. It stays in this tab only.</p>
    <label for="token">Admin token</label>
    <input id="token" name="token" type="password" inputmode="text" spellcheck="false" required minlength="32" maxlength="32">
    <p class="muted" id="unlock-error" role="alert"></p>
    <div class="row"><button class="primary" type="submit">Unlock</button></div>
  </form>

  <section class="card deck" id="deck" hidden aria-labelledby="deck-title">
    <h2 id="deck-title" class="muted" style="margin:0">Live</h2>
    <div class="state" id="state" data-state="off" aria-live="polite">
      <span class="lamp" aria-hidden="true"></span><strong id="state-text">OFF</strong><span class="muted" id="state-more"></span>
    </div>
    <div class="meter" id="meter" role="meter" aria-label="Level above the quiet" aria-valuemin="0" aria-valuemax="60" aria-valuenow="0"></div>
    <div class="row">
      <button type="button" id="arm" aria-pressed="false">Voice trigger</button>
      <button type="button" class="primary" id="rec">Record</button>
      <button type="button" id="stop" disabled>Stop</button>
    </div>
    <div class="facts" id="facts"></div>
  </section>

  <section class="card" id="clips-card" hidden aria-labelledby="clips-title">
    <h2 id="clips-title">Clips</h2>
    <p class="muted">Kept in the board's RAM: they vanish when it restarts. Download the ones you want to keep.</p>
    <div class="speed">
      <label for="speed">Speed on the board's speaker</label>
      <select id="speed">
        <option value="0.5">0.5x (giant)</option><option value="0.75">0.75x</option><option value="1" selected>1x</option>
        <option value="1.5">1.5x</option><option value="2">2x (chipmunk)</option>
      </select>
    </div>
    <ol id="clips"></ol>
    <p class="muted empty" id="empty">No clips yet. Turn on the voice trigger and say something.</p>
  </section>
</main>

<template id="clip-tpl">
  <li class="clip card">
    <header><h3></h3><span class="muted when"></span></header>
    <canvas aria-label="Waveform: tap to play from that point" role="img"></canvas>
    <progress class="board-progress" max="1" value="0" hidden aria-label="Playing on the board"></progress>
    <div class="row">
      <button type="button" class="play">Play</button>
      <button type="button" class="board">Play on board</button>
      <a class="button download" href="#" download>Download</a>
      <button type="button" class="delete">Delete</button>
      <span class="muted info"></span>
    </div>
  </li>
</template>

<script>
"use strict";
const $ = (s, root = document) => root.querySelector(s);
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
let token = sessionStorage.getItem("s4-token") || "";
let ws = null, retry = null, last = null;
let ctx = null, playing = null;  // { n, src, startedAt, offset }
const clips = new Map();         // n -> { el, buffer, url, info }

for (let i = 0; i < 30; i++) $("#meter").append(document.createElement("i"));
const trig = document.createElement("span");
trig.className = "trig"; trig.setAttribute("aria-hidden", "true");
$("#meter").append(trig);

function lock(message) {
  token = ""; sessionStorage.removeItem("s4-token");
  if (ws) { ws.onclose = null; ws.close(); ws = null; }
  $("#unlock").hidden = false; $("#deck").hidden = true; $("#clips-card").hidden = true;
  $("#unlock-error").textContent = message || "";
  $("#link").textContent = "locked";
  for (const c of clips.values()) URL.revokeObjectURL(c.url);
  clips.clear(); $("#clips").replaceChildren();
}

async function api(path, method = "GET") {
  const r = await fetch(path, { method, headers: { Authorization: "Bearer " + token } });
  if (r.status === 401) { lock("That token didn't work. Type token in the serial monitor for the current one."); throw new Error("locked"); }
  if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
  return r;
}

function connect() {
  clearTimeout(retry);
  $("#link").textContent = "connecting...";
  ws = new WebSocket(`ws://${location.host}/ws?token=${encodeURIComponent(token)}`);
  ws.onopen = () => { $("#unlock").hidden = true; $("#deck").hidden = false; $("#clips-card").hidden = false; $("#link").textContent = "live"; };
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.type === "status") showStatus(m.data);
    if (m.type === "clips") showClips(m.data);
  };
  ws.onclose = (e) => {
    if (e.code === 4001) return lock("That token didn't work. Type token in the serial monitor for the current one.");
    $("#link").textContent = "reconnecting...";
    retry = setTimeout(connect, 2000);
  };
}

function showStatus(s) {
  last = s;
  const st = $("#state");
  st.dataset.state = s.state;
  $("#state-text").textContent = s.state === "recording" ? "REC" : s.state.toUpperCase();
  $("#state-more").textContent = s.state === "recording" ? `clip ${s.clip} · ${(s.recMs / 1000).toFixed(1)} s${s.manual ? " · by hand" : ""}`
    : s.armed ? `starts at ${s.trigger} dB above the quiet` : "press Record, or turn on the voice trigger";
  const lit = Math.round(Math.min(1, s.above / 60) * 30);
  [...$("#meter").children].forEach((el, i) => {
    if (el.tagName !== "I") return;
    el.className = i < lit ? `on${i >= 20 ? " hot" : i >= 12 ? " loud" : ""}` : "";
  });
  $("#meter").setAttribute("aria-valuenow", String(Math.round(s.above)));
  trig.style.left = `${Math.min(1, s.trigger / 60) * 100}%`;
  $("#arm").setAttribute("aria-pressed", String(s.armed));
  $("#mic").textContent = `S4 · ${s.mic}`;
  for (const [n, entry] of clips) {
    const on = s.playing === n, bar = $(".board-progress", entry.el);
    bar.hidden = !on;
    if (on) bar.value = s.playProgress;
    $(".board", entry.el).textContent = on ? "Stop on board" : "Play on board";
    $(".board", entry.el).disabled = entry.info.open || s.state === "recording";
  }
  $("#rec").disabled = s.state === "recording";
  $("#stop").disabled = s.state !== "recording";
  $("#facts").replaceChildren(...[
    `${s.clips} clip${s.clips === 1 ? "" : "s"} · ${(s.usedBytes / 1048576).toFixed(1)} of ${(s.budgetBytes / 1048576).toFixed(0)} MB`,
    `hold ${(s.holdMs / 1000).toFixed(1)} s · max ${s.maxS} s · pre-roll ${(s.prerollMs / 1000).toFixed(1)} s`,
    `${(s.rate / 1000).toFixed(0)} kHz · quiet at ${s.floorDb.toFixed(0)} dBFS`,
  ].map((t) => Object.assign(document.createElement("span"), { textContent: t })));
}

function when(c) {
  if (!c.wall) return "";
  return new Date(c.wall * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function showClips(list) {
  const seen = new Set();
  for (const c of list) {
    seen.add(c.n);
    let entry = clips.get(c.n);
    if (!entry) {
      const el = $("#clip-tpl").content.firstElementChild.cloneNode(true);
      entry = { el, buffer: null, url: null, info: c, loading: false };
      clips.set(c.n, entry);
      $(".play", el).onclick = () => toggle(c.n);
      $(".delete", el).onclick = () => remove(c.n);
      $(".board", el).onclick = () => command(last && last.playing === c.n ? "/api/stop" : `/api/play?n=${c.n}&speed=${$("#speed").value}`);
      $("canvas", el).onclick = (e) => {
        const r = e.currentTarget.getBoundingClientRect();
        play(c.n, ((e.clientX - r.left) / r.width) * (entry.buffer ? entry.buffer.duration : 0));
      };
      $("#clips").prepend(el);
    }
    entry.info = c;
    $("h3", entry.el).textContent = `Clip ${c.n}${c.open ? " · recording" : ""}`;
    $(".when", entry.el).textContent = when(c);
    $(".info", entry.el).textContent = `${(c.ms / 1000).toFixed(1)} s · ${c.manual ? "by hand" : "voice"} · x${c.gain} louder`;
    $(".download", entry.el).download = c.name;
    for (const b of entry.el.querySelectorAll("button, a")) b.toggleAttribute("disabled", c.open);
    if (!c.open && !entry.buffer && !entry.loading) load(c.n);
  }
  for (const [n, entry] of clips) if (!seen.has(n)) { if (playing && playing.n === n) stop(); URL.revokeObjectURL(entry.url); entry.el.remove(); clips.delete(n); }
  $("#empty").hidden = list.length > 0;
}

async function load(n) {
  const entry = clips.get(n);
  entry.loading = true;
  try {
    const blob = await (await api(`/api/clip?n=${n}`)).blob();
    entry.url = URL.createObjectURL(blob);
    $(".download", entry.el).href = entry.url;
    ctx = ctx || new (window.AudioContext || window.webkitAudioContext)();
    entry.buffer = await ctx.decodeAudioData(await blob.arrayBuffer());
    draw(n, 0);
  } catch (err) {
    $(".info", entry.el).textContent = `could not load: ${err.message}`;
  } finally { entry.loading = false; }
}

// The waveform: for each pixel column, the lowest and highest sample that falls in it.
function draw(n, position) {
  const entry = clips.get(n);
  if (!entry || !entry.buffer) return;
  const canvas = $("canvas", entry.el), dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.round(canvas.clientHeight * dpr);
  if (canvas.width !== w) canvas.width = w;
  if (canvas.height !== h) canvas.height = h;
  const g = canvas.getContext("2d"), data = entry.buffer.getChannelData(0), per = data.length / w;
  const playedTo = (position / entry.buffer.duration) * w;
  g.clearRect(0, 0, w, h);
  for (let x = 0; x < w; x++) {
    let lo = 1, hi = -1;
    for (let i = Math.floor(x * per), end = Math.floor((x + 1) * per); i < end; i++) { const v = data[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi < lo) continue;
    g.fillStyle = x < playedTo ? css("--wave-played") : css("--wave");
    g.fillRect(x, ((1 - hi) / 2) * h, 1, Math.max(1, ((hi - lo) / 2) * h));
  }
  if (position > 0) { g.fillStyle = css("--playhead"); g.fillRect(Math.round(playedTo), 0, Math.max(1, dpr), h); }
}

function play(n, offset = 0) {
  const entry = clips.get(n);
  if (!entry || !entry.buffer) return;
  stop();
  ctx.resume();
  const src = ctx.createBufferSource();
  src.buffer = entry.buffer;
  src.connect(ctx.destination);
  src.start(0, Math.max(0, Math.min(offset, entry.buffer.duration - 0.01)));
  playing = { n, src, startedAt: ctx.currentTime - offset };
  src.onended = () => { if (playing && playing.src === src) stop(); };
  $(".play", entry.el).textContent = "Pause";
  const frame = () => {
    if (!playing || playing.src !== src) return;
    draw(n, ctx.currentTime - playing.startedAt);
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

function stop() {
  if (!playing) return;
  const { n, src } = playing;
  playing = null;
  try { src.stop(); } catch (e) { /* already ended */ }
  const entry = clips.get(n);
  if (entry) { $(".play", entry.el).textContent = "Play"; draw(n, 0); }
}

function toggle(n) { if (playing && playing.n === n) stop(); else play(n); }

async function remove(n) {
  if (!confirm(`Delete clip ${n}? It is only on the board, so it's gone for good.`)) return;
  try { await api(`/api/clip?n=${n}`, "DELETE"); } catch (err) { if (err.message !== "locked") alert(err.message); }
}

async function command(path) {
  try { await api(path, "POST"); } catch (err) { if (err.message !== "locked") $("#state-more").textContent = err.message; }
}

$("#arm").onclick = () => command(`/api/arm?on=${last && last.armed ? 0 : 1}`);
$("#rec").onclick = () => command("/api/rec");
$("#stop").onclick = () => command("/api/stop");
$("#unlock").onsubmit = (e) => {
  e.preventDefault();
  token = $("#token").value.trim();
  sessionStorage.setItem("s4-token", token);
  $("#token").value = "";
  connect();
};
window.addEventListener("resize", () => { for (const n of clips.keys()) if (!playing || playing.n !== n) draw(n, 0); });
if (token) connect();
</script>
</body>
</html>
)HTML";
