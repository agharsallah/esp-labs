// The built-in phone page, served at http://bench.local/ (or http://192.168.4.1/ on the set-up
// network). Plain HTML + JS, no libraries, so it fits in flash and works without internet.
// It uses the same REST API and WebSocket as the desktop dashboard in esp-labs/apps/dashboard.
#pragma once

static const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bench</title>
<style>
:root{--bg:#f2f4f3;--card:#fff;--ink:#17201d;--muted:#5b6964;--line:#d3dbd7;--accent:#b0601f;--ok:#1f7a4a;--warn:#a05a00;--bad:#b3261e;color-scheme:light}
@media (prefers-color-scheme:dark){:root{--bg:#0e1412;--card:#161e1b;--ink:#e4ebe7;--muted:#9aaba4;--line:#2b3833;--accent:#e39a5c;--ok:#5fcf8f;--warn:#f0b85a;--bad:#ff8a80;color-scheme:dark}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,sans-serif;padding:16px}
main{max-width:560px;margin:0 auto;display:grid;gap:12px}h1{font-size:22px;margin:0}h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:0 0 8px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-variant-numeric:tabular-nums}.kv dt{color:var(--muted)}.kv dd{margin:0}
button,input,select{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px 12px}button{cursor:pointer}button.on{background:var(--ink);color:var(--card)}button:focus-visible,input:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
input[type=text],input[type=password]{flex:1;min-width:0}input[type=range]{flex:1}.dot{width:10px;height:10px;border-radius:50%;display:inline-block;background:var(--line)}
.muted{color:var(--muted);font-size:13px}#feed{list-style:none;margin:0;padding:0;max-height:140px;overflow:auto;font:13px ui-monospace,monospace}
table{width:100%;border-collapse:collapse;font-size:14px}td{padding:4px 6px;border-bottom:1px solid var(--line)}td.n{text-align:right;font-variant-numeric:tabular-nums}
</style></head><body><main>
<header class="row"><span class="dot" id="link"></span><h1>Bench</h1><span class="muted" id="sub">connecting…</span></header>
<section id="setup" hidden><h2>Set up Wi-Fi</h2><p class="muted">Pick your network and type its password. It is stored on the board only.</p>
<div class="row"><select id="ssid" aria-label="Network"></select><button id="rescan" type="button">Scan</button></div>
<div class="row" style="margin-top:8px"><input id="pass" type="password" placeholder="Password" aria-label="Password" autocomplete="current-password"><button id="save" type="button">Join</button></div></section>
<section><h2>Status</h2><dl class="kv" id="kv"></dl></section>
<section><h2>Screen</h2><div class="row" id="views"></div>
<div class="row" style="margin-top:10px"><label for="bl" class="muted">Brightness</label><input id="bl" type="range" min="0" max="255"></div>
<div class="row" style="margin-top:10px"><button id="auto" type="button">Auto-scan</button><button id="scan" type="button">Scan now</button></div></section>
<section><h2>Message on the screen</h2><div class="row"><input id="msg" type="text" maxlength="96" placeholder="Type something" aria-label="Message"><button id="send" type="button">Show</button></div></section>
<section><h2>On-board LED</h2><div class="row"><input id="led" type="color" aria-label="LED colour" value="#000000"><button id="ledoff" type="button">Off</button></div></section>
<section><h2>Networks</h2><table id="nets"></table></section>
<section><h2>Live events</h2><ul id="feed"></ul></section>
<section><h2>Admin</h2><p class="muted">Needs the token printed on serial (command: token). Kept in this browser only.</p>
<div class="row"><input id="tok" type="password" placeholder="Token" aria-label="Admin token"><button id="reboot" type="button">Reboot</button><button id="forget" type="button">Forget Wi-Fi</button></div>
<div class="row" style="margin-top:8px"><input id="fw" type="file" accept=".bin" aria-label="Firmware file"><button id="ota" type="button">Update firmware</button></div><p class="muted" id="otamsg"></p></section>
</main><script>
const $=id=>document.getElementById(id);let S=null;const esc=t=>String(t).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const tok=()=>{const t=$('tok').value.trim();try{localStorage.setItem('tok',t)}catch(e){}return t};
try{$('tok').value=localStorage.getItem('tok')||''}catch(e){}
async function api(method,path,body,admin){const h={};if(body)h['Content-Type']='application/json';if(admin)h.Authorization='Bearer '+tok();
 const r=await fetch(path,{method,headers:h,body:body?JSON.stringify(body):undefined});const j=await r.json().catch(()=>({}));if(!r.ok)note('⚠ '+(j.error||r.status));return r.ok}
function note(t){const li=document.createElement('li');li.textContent=new Date().toLocaleTimeString()+'  '+t;$('feed').prepend(li)}
function status(s){S=s;const w=s.wifi;$('link').style.background=w.state==='connected'?'var(--ok)':w.state==='connecting'?'var(--warn)':w.state==='failed'?'var(--bad)':'var(--line)';
 $('sub').textContent=w.state==='connected'?w.ssid+' · '+w.rssi+' dBm':w.state;$('setup').hidden=w.state==='connected';
 const kv={'IP':w.ip||'—','Name':w.hostname,'Channel':w.channel||'—','Chip':(s.tempC??0).toFixed(1)+' °C','Uptime':Math.floor(s.uptimeS/60)+' min','Free RAM':Math.round(s.heapFree/1024)+' KB','Clients':s.clients};
 $('kv').innerHTML=Object.entries(kv).map(([k,v])=>`<dt>${k}</dt><dd>${esc(v)}</dd>`).join('');
 const sc=s.screen;$('views').innerHTML=sc.views.map(v=>`<button type="button" class="${v===sc.view?'on':''}" data-v="${v}">${v}</button>`).join('');
 if(document.activeElement!==$('bl'))$('bl').value=sc.backlight;$('auto').className=sc.autoScan?'on':'';}
function networks(n){$('nets').innerHTML=n.networks.slice(0,12).map(x=>`<tr><td>${x.ssid?esc(x.ssid):'(hidden)'}</td><td class="n">${x.rssi} dBm</td><td class="n">ch ${x.channel}</td><td>${esc(x.auth)}</td></tr>`).join('');
 const sel=$('ssid'),cur=sel.value;sel.innerHTML=n.networks.filter(x=>x.ssid).map(x=>`<option value="${esc(x.ssid)}">${esc(x.ssid)}</option>`).join('');if(cur)sel.value=cur}
function connect(){const ws=new WebSocket(`ws://${location.host}/ws`);ws.onmessage=e=>{const m=JSON.parse(e.data);
 if(m.type==='status')status(m.data);else if(m.type==='networks')networks(m.data);else if(m.type==='event')note(m.data.source+': '+m.data.gesture);else if(m.type==='ota')$('otamsg').textContent='Updating… '+m.data.percent+'%'};
 ws.onclose=()=>{$('link').style.background='var(--line)';$('sub').textContent='reconnecting…';setTimeout(connect,2000)}}
$('views').onclick=e=>{const v=e.target.dataset.v;if(v)api('PUT','/api/view',{view:v})};
$('bl').onchange=e=>api('PUT','/api/settings',{backlight:+e.target.value});
$('auto').onclick=()=>api('PUT','/api/settings',{autoScan:!(S&&S.screen.autoScan)});
$('scan').onclick=$('rescan').onclick=()=>api('POST','/api/scan');
$('send').onclick=()=>api('PUT','/api/message',{text:$('msg').value});
$('led').oninput=e=>{const h=e.target.value;api('PUT','/api/led',{r:parseInt(h.slice(1,3),16),g:parseInt(h.slice(3,5),16),b:parseInt(h.slice(5,7),16)})};
$('ledoff').onclick=()=>{$('led').value='#000000';api('PUT','/api/led',{r:0,g:0,b:0})};
$('save').onclick=async()=>{if(await api('PUT','/api/wifi',{ssid:$('ssid').value,password:$('pass').value},true))note('Saved. The board is joining…')};
$('reboot').onclick=()=>api('POST','/api/reboot',null,true);$('forget').onclick=()=>api('DELETE','/api/wifi',null,true);
$('ota').onclick=async()=>{const f=$('fw').files[0];if(!f)return;const fd=new FormData();fd.append('firmware',f);$('otamsg').textContent='Uploading…';
 const r=await fetch('/api/ota',{method:'POST',headers:{Authorization:'Bearer '+tok()},body:fd});const j=await r.json().catch(()=>({}));$('otamsg').textContent=r.ok?'Done. The board is rebooting into the new firmware.':'Failed: '+(j.error||r.status)};
fetch('/api/networks').then(r=>r.ok&&r.json()).then(n=>n&&networks(n));connect();
</script></body></html>)HTML";
