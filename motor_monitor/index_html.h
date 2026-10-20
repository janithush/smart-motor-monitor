/*
  =====================================================================
  index_html.h - Single-page dashboard embedded in flash (PROGMEM)
  =====================================================================
  LAVISH / AXIO industrial styling concept:
    - Obsidian #0b0f19 background, neon cyan/emerald telemetry accents,
      amber/crimson trip warnings, monospace instrument typography.
    - 100% self-contained: zero CDN, zero external fonts. Renders
      correctly on the ESP32's AP hotspot with no internet at all.

  Served verbatim by GET / . The JS polls `/api/status` every ~1.7s:
    { up, heap, mOn, st, stRem, fl, fault,
      wifi:{mode,ip,rssi}, tg:{ok},
      m:{t,tRise,i,v,vib,vibMg},
      lim:{t,tRise,i,iPlate,vLow,vHigh,vib},
      events:[{sq,up,tp,tx}, ... newest first] }
  Control actions POST to `/api/reset` and `/api/motor` and follow the
  exact same safety-interlocked actuation path as the physical buttons.
  =====================================================================
*/

#ifndef INDEX_HTML_H
#define INDEX_HTML_H

#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SMART INDUCTION MOTOR MONITOR</title>
<style>
:root{
  --bg:#0b0f19; --panel:#111827; --panel2:#0e1420; --line:#1e293b; --line2:#243049;
  --txt:#e2e8f0; --mut:#7c8db0; --cy:#22d3ee; --em:#34d399; --am:#fbbf24; --cr:#f43f5e;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);min-height:100vh;
  font:14px/1.5 ui-monospace,'Cascadia Code',Consolas,Menlo,monospace}
header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;justify-content:space-between;
  padding:14px 22px;border-bottom:1px solid var(--line)}
.brand{display:flex;gap:14px;align-items:center}
.logo{width:46px;height:46px;border-radius:10px;flex:none;
  background:linear-gradient(135deg,#22d3ee22,#34d39922);border:1px solid var(--line2);
  display:flex;align-items:center;justify-content:center;font-weight:800;color:var(--cy)}
.logo span{transform:scaleY(-1) rotate(8deg)}
h1{font-size:17px;letter-spacing:2px}
.spec{color:var(--mut);font-size:11px;letter-spacing:1px;margin-top:2px}
.badges{display:flex;gap:8px;flex-wrap:wrap}
.badge{border:1px solid var(--line2);background:var(--panel2);padding:6px 10px;
  border-radius:999px;font-size:11px;color:var(--mut);letter-spacing:1px}
.badge b{color:var(--txt);font-weight:600}
.badge.ok{border-color:#34d39955;color:#a7f3d0}
.badge.ok b{color:var(--em)}
.badge.warn{border-color:#fbbf2455;color:#fde68a}
.badge.warn b{color:var(--am)}
.badge.off{border-color:#f43f5e55;color:#fda4af}
.badge.off b{color:var(--cr)}
main{max-width:1080px;margin:0 auto;padding:20px 22px 30px;
  display:flex;flex-direction:column;gap:18px}
.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
.card .lab{font-size:10px;letter-spacing:2px;color:var(--mut)}
.card .val{font-size:20px;font-weight:700;margin-top:6px;letter-spacing:1px}
.card .sub{font-size:11px;color:var(--mut);margin-top:4px;min-height:16px}
.val.ok{color:var(--em)}.val.warn{color:var(--am)}.val.crit{color:var(--cr)}.val.idle{color:var(--mut)}
.gauges{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}
.gauge{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
.ghead{display:flex;justify-content:space-between;align-items:baseline}
.gname{font-size:10px;letter-spacing:2px;color:var(--mut)}
.gval{font-size:18px;font-weight:700}
.gval small{font-size:11px;color:var(--mut);font-weight:400}
.gbar{position:relative;height:14px;margin-top:10px;background:var(--panel2);
  border:1px solid var(--line2);border-radius:7px}
.gfill{position:absolute;left:0;top:0;bottom:0;border-radius:6px;width:0%;
  background:linear-gradient(90deg,#0ea5e9,#22d3ee);transition:width .5s}
.gfill.warn{background:linear-gradient(90deg,#b45309,#fbbf24)}
.gfill.crit{background:linear-gradient(90deg,#be123c,#f43f5e)}
.gmark{position:absolute;top:-4px;bottom:-4px;width:2px;background:#f8fafc55;
  box-shadow:0 0 4px #f8fafc66}
.gfoot{display:flex;justify-content:space-between;font-size:10px;
  color:var(--mut);margin-top:5px;letter-spacing:1px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px 16px}
.panel h2{font-size:11px;letter-spacing:2px;color:var(--mut);margin-bottom:10px}
.twrap{overflow-x:auto;max-height:260px;overflow-y:auto;border:1px solid var(--line);
  border-radius:8px}
table{width:100%;border-collapse:collapse;font-size:12px}
thead tr{position:sticky;top:0;background:#0e1420}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--line);white-space:nowrap}
th{font-size:10px;letter-spacing:1.5px;color:var(--mut)}
td.ev{white-space:normal}
.chip{display:inline-block;padding:2px 8px;border-radius:999px;font-size:10px;
  letter-spacing:1px;border:1px solid}
.chip.TRIP{color:#fda4af;border-color:#f43f5e66;background:#f43f5e14}
.chip.ON{color:#67e8f9;border-color:#22d3ee55;background:#22d3ee14}
.chip.START{color:#6ee7b7;border-color:#34d39955;background:#34d39914}
.chip.RESET{color:#fcd34d;border-color:#fbbf2455;background:#fbbf2414}
.chip.OFF{color:#94a3b8;border-color:#64748b55}
.chip.WIFI,.chip.AP,.chip.BOOT,.chip.INFO{color:#a5b4fc;border-color:#818cf855}
.controls{display:flex;gap:10px;flex-wrap:wrap}
.ctrl{appearance:none;font:inherit;font-size:12px;letter-spacing:1.5px;padding:12px 18px;
  border-radius:10px;border:1px solid;cursor:pointer;background:var(--panel2);
  color:var(--txt);text-decoration:none;display:inline-flex;align-items:center}
.ctrl.ok{border-color:#34d39966;color:#6ee7b7}
.ctrl.ok:hover:not(:disabled){background:#34d3991c}
.ctrl.danger{border-color:#f43f5e66;color:#fda4af}
.ctrl.danger:hover:not(:disabled){background:#f43f5e1c}
.ctrl.amber{border-color:#fbbf2466;color:#fcd34d}
.ctrl.amber:hover:not(:disabled){background:#fbbf241c}
.ctrl.azure{border-color:#22d3ee55;color:#67e8f9}
.ctrl.azure:hover{background:#22d3ee14}
.ctrl.violet{border-color:#a78bfa55;color:#c4b5fd}
.ctrl.violet:hover{background:#a78bfa14}
.ctrl:disabled{opacity:.35;cursor:not-allowed}
.hint{font-size:10px;color:var(--mut);letter-spacing:1px}
footer{display:flex;gap:22px;justify-content:center;padding:14px;font-size:10px;
  color:var(--mut);letter-spacing:1.5px;border-top:1px solid var(--line);flex-wrap:wrap}
.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(20px);
  background:#111827;border:1px solid var(--line2);color:var(--txt);
  padding:10px 16px;border-radius:10px;font-size:12px;opacity:0;pointer-events:none;
  transition:.25s;z-index:9}
.toast.show{opacity:1;transform:translateX(-50%)}
.toast.crit{border-color:#f43f5e66;color:#fda4af}
@media(max-width:760px){.cards,.gauges{grid-template-columns:1fr}}
</style>
</head>
<body>

<header>
  <div class="brand">
    <div class="logo"><span>&#9099;</span></div>
    <div>
      <h1>SMART INDUCTION MOTOR MONITOR</h1>
      <div class="spec">230 V AC &middot; 50 Hz &middot; 0.55 kW &middot; 4.1 A &middot; 2850 RPM &middot; ESP32</div>
    </div>
  </div>
  <div class="badges">
    <span class="badge off" id="bWifi">WI-FI <b id="bWifiT">--</b></span>
    <span class="badge off" id="bTg">TELEGRAM <b id="bTgT">--</b></span>
    <span class="badge" id="bIp">IP <b id="bIpT">--</b></span>
  </div>
</header>

<main>

  <section class="cards">
    <div class="card">
      <div class="lab">MOTOR CONDITION</div>
      <div class="val idle" id="cCond">--</div>
      <div class="sub" id="cCondSub"></div>
    </div>
    <div class="card">
      <div class="lab">MOTOR SUPPLY</div>
      <div class="val idle" id="cSup">--</div>
      <div class="sub" id="cSupSub"></div>
    </div>
    <div class="card">
      <div class="lab">LATCH STATE</div>
      <div class="val idle" id="cLatch">--</div>
      <div class="sub" id="cLatchSub"></div>
    </div>
  </section>

  <section class="gauges">
    <div class="gauge">
      <div class="ghead"><span class="gname">WINDING TEMPERATURE</span>
        <span class="gval"><b id="gT">--</b><small> &deg;C</small></span></div>
      <div class="gbar" id="barT"><div class="gfill" id="fT"></div></div>
      <div class="gfoot"><span>0</span><span id="gTl"></span><span>150 &deg;C</span></div>
    </div>
    <div class="gauge">
      <div class="ghead"><span class="gname">MOTOR CURRENT</span>
        <span class="gval"><b id="gI">--</b><small> A</small></span></div>
      <div class="gbar" id="barI"><div class="gfill" id="fI"></div></div>
      <div class="gfoot"><span>0</span><span id="gIl"></span><span>8 A</span></div>
    </div>
    <div class="gauge">
      <div class="ghead"><span class="gname">SUPPLY VOLTAGE</span>
        <span class="gval"><b id="gV">--</b><small> V</small></span></div>
      <div class="gbar" id="barV"><div class="gfill" id="fV"></div></div>
      <div class="gfoot"><span>150</span><span id="gVl"></span><span>280 V</span></div>
    </div>
    <div class="gauge">
      <div class="ghead"><span class="gname">VIBRATION (RMS)</span>
        <span class="gval"><b id="gVib">--</b><small> mm/s</small></span></div>
      <div class="gbar" id="barVib"><div class="gfill" id="fVib"></div></div>
      <div class="gfoot"><span>0</span><span id="gVibl"></span><span>6 mm/s</span></div>
    </div>
  </section>

  <section class="panel">
    <h2>EVENT LOG</h2>
    <div class="twrap">
      <table>
        <thead><tr><th>TIME</th><th>TYPE</th><th>EVENT</th></tr></thead>
        <tbody id="evBody"></tbody>
      </table>
    </div>
    <div class="gfoot" id="evEmpty" style="display:none;margin-top:8px">&middot; no events yet &middot;</div>
  </section>

  <section class="panel">
    <h2>CONTROL ACTIONS</h2>
    <div class="controls">
      <button class="ctrl ok"    id="btnOn">MOTOR ON</button>
      <button class="ctrl danger" id="btnOff">MOTOR OFF</button>
      <button class="ctrl amber" id="btnReset">RESET FAULT</button>
      <a class="ctrl azure"  href="/api/log" download="motor_log.csv">EXPORT CSV</a>
      <a class="ctrl violet" href="/api/log?fmt=html" target="_blank" rel="noopener">PRINT / PDF LOG</a>
    </div>
    <div class="hint" style="margin-top:10px">
      Reset clears the fault latch only. MOTOR ON runs the 5 s soft-start with
      vibration-grace interlocks. PDF log: open printable page, then Ctrl+P.
    </div>
  </section>

</main>

<footer>
  <span id="fUp">UPTIME --</span>
  <span id="fHeap">HEAP --</span>
  <span id="fRssi">RSSI --</span>
  <span>POLL 1.7 s</span>
</footer>

<div class="toast" id="toast"></div>

<script>
const $ = id => document.getElementById(id);
let S = null, failCount = 0, marksDone = false;

const fmtUp = s => {
  s = Math.max(0, s | 0);
  const h = (s / 3600) | 0, m = ((s % 3600) / 60) | 0, d = s % 60;
  return (h ? h + 'h ' : '') + ((h || m) ? m + 'm ' : '') + d + 's';
};

function gState(k, v) {
  const L = S.lim || {};
  if (!S.lim) return 'ok';
  if (k === 't')   return v > L.t        ? 'crit' : v > L.t * 0.9        ? 'warn' : 'ok';
  if (k === 'i')   return v > L.i        ? 'crit' : v > L.iPlate         ? 'warn' : 'ok';
  if (k === 'vib') return v > L.vib      ? 'crit' : v > L.vib * 0.8      ? 'warn' : 'ok';
  if (k === 'v') {
    if (v > L.vHigh || v < L.vLow) return 'crit';
    return (v > L.vHigh * 0.97 || (v < L.vLow * 1.03 && v > 20)) ? 'warn' : 'ok';
  }
  return 'ok';
}

const clamp01 = x => Math.min(1, Math.max(0, x));

function drawMark(barId, frac) {
  $(barId).insertAdjacentHTML('beforeend',
    '<div class="gmark" style="left:' + (clamp01(frac) * 100).toFixed(1) + '%"></div>');
}

// Trip limit markers are drawn once per page load, positioned from the
// live /api/status limits so UI always mirrors firmware thresholds.
function setMarks() {
  if (marksDone || !S.lim) return;
  marksDone = true;
  const L = S.lim;
  drawMark('barT',   L.t / 150);
  drawMark('barI',   L.iPlate / 8);
  drawMark('barI',   L.i / 8);
  drawMark('barV',   (L.vLow - 150) / 130);
  drawMark('barV',   (L.vHigh - 150) / 130);
  drawMark('barVib', L.vib / 6);
  $('gTl').textContent   = 'trip > ' + L.t + ' °C';
  $('gIl').textContent   = 'plate ' + L.iPlate + ' / trip ' + L.i + ' A';
  $('gVl').textContent   = L.vLow + ' – ' + L.vHigh + ' V';
  $('gVibl').textContent = 'trip > ' + L.vib + ' mm/s';
}

function setGauge(id, kf, fillId, barId, k, v, scale) {
  const st = gState(k, v);
  const g = $(id);
  g.textContent = fmtVal(k, v);
  const f = $(fillId);
  f.style.width = (clamp01(v / scale) * 100).toFixed(1) + '%';
  f.className = 'gfill' + (st === 'crit' ? ' crit' : st === 'warn' ? ' warn' : '');
  g.parentElement.parentElement.querySelector('.gname').style.color =
    st === 'crit' ? '#fda4af' : st === 'warn' ? '#fde68a' : '';
}
const fmtVal = (k, v) =>
  k === 't' ? v.toFixed(1) : k === 'i' ? v.toFixed(2) : k === 'v' ? v.toFixed(0) : v.toFixed(2);

function setCard(id, subId, text, sub, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = 'val ' + cls;
  $(subId).textContent = sub || '';
}

function render() {
  if (!S) return;
  setMarks();

  // connection badges
  const sta = S.wifi.mode === 'STA';
  $('bWifiT').textContent = sta ? 'LINKED' : 'AP HOTSPOT';
  $('bWifi').className = 'badge ' + (sta ? 'ok' : 'warn');
  $('bTgT').textContent = S.tg.ok ? 'LINKED' : 'OFFLINE';
  $('bTg').className = 'badge ' + (S.tg.ok ? 'ok' : 'off');
  $('bIpT').textContent = S.wifi.ip || '--';

  // status cards
  if (S.fl) {
    setCard('cCond', 'cCondSub', 'FAULT', S.fault || 'fault latched', 'crit');
  } else if (S.st) {
    setCard('cCond', 'cCondSub', 'STARTING', 'soft-start in progress', 'warn');
  } else if (S.mOn) {
    setCard('cCond', 'cCondSub', 'NORMAL', 'all parameters within limits', 'ok');
  } else {
    setCard('cCond', 'cCondSub', 'STOPPED', 'supply interrupted', 'idle');
  }
  if (S.st) {
    const rem = S.stRem !== undefined ? Math.max(0, Math.ceil(S.stRem / 1000)) : null;
    setCard('cSup', 'cSupSub', 'SOFT-START', rem !== null ? rem + ' s to energize' : '', 'warn');
  } else if (S.mOn) {
    setCard('cSup', 'cSupSub', 'ON', 'contactor energized', 'ok');
  } else {
    setCard('cSup', 'cSupSub', 'OFF', 'contactor open', 'idle');
  }
  if (S.fl) {
    setCard('cLatch', 'cLatchSub', 'RESET NEEDED', 'latched - clear to continue', 'crit');
  } else {
    setCard('cLatch', 'cLatchSub', 'CLEAR', 'no active fault', 'ok');
  }

  // gauges
  setGauge('gT', 't', 'fT', 'barT', 't', S.m.t, 150);
  setGauge('gI', 'i', 'fI', 'barI', 'i', S.m.i, 8);
  setGauge('gV', 'v', 'fV', 'barV', 'v', S.m.v, 280);
  setGauge('gVib', 'vib', 'fVib', 'barVib', 'vib', S.m.vib, 6);

  // event table (newest first)
  $('evBody').innerHTML = (S.events || []).map(e =>
    '<tr><td>' + fmtUp(e.up) + '</td><td><span class="chip ' + e.tp + '">' + e.tp +
    '</span></td><td class="ev">' + e.tx + '</td></tr>').join('');
  $('evEmpty').style.display = (S.events && S.events.length) ? 'none' : 'block';

  // interlocked button availability (mirrors firmware interlocks)
  $('btnOn').disabled = !!(S.fl || S.mOn || S.st);
  $('btnOff').disabled = !(S.mOn || S.st);
  $('btnReset').disabled = !S.fl;

  // footer
  $('fUp').textContent = 'UPTIME ' + fmtUp(S.up);
  $('fHeap').textContent = 'HEAP ' + Math.round(S.heap / 1024) + ' KB';
  $('fRssi').textContent = 'RSSI ' + S.wifi.rssi + ' dBm';
}

async function poll() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    S = await r.json();
    failCount = 0;
    render();
  } catch (e) {
    if (++failCount >= 2) {
      $('bWifiT').textContent = 'RECONNECTING';
      $('bTgT').textContent = '--';
      failCount = 1;
    }
  }
}

let toastTimer;
function toast(msg, crit) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'toast show' + (crit ? ' crit' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('show'), 2600);
}

async function act(path, body) {
  let r;
  try {
    r = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: body
    });
  } catch (e) { toast('Network error - request not sent', true); return; }
  let j = {};
  try { j = await r.json(); } catch (_) {}
  toast(j.message || ('HTTP ' + r.status), !r.ok);
  if (r.ok) poll();
}

$('btnOn').onclick    = () => act('/api/motor', '{"action":"on"}');
$('btnOff').onclick   = () => act('/api/motor', '{"action":"off"}');
$('btnReset').onclick = () => act('/api/reset', '{}');

poll();
setInterval(poll, 1700);
</script>
</body>
</html>
)HTML";

#endif // INDEX_HTML_H
