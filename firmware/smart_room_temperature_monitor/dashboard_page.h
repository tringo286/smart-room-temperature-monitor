// Web dashboard served by the NodeMCU at http://<board-ip>/
//
// Self-contained (no external libraries or fonts) so it works without
// internet access. It polls /api/status every 2 s and /api/history every
// 10 s, and changes settings with POST /api/threshold and /api/mute.
//
// To work on this page without flashing the board, run:
//   python3 tools/preview_dashboard.py
#pragma once

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Room Monitor</title>
<style>
:root{
  --bg:#eef1f6;--card:#fff;--ink:#141b2d;--muted:#5d6782;--line:#dfe4ee;--track:#e6eaf2;
  --ok:#14915a;--ok-bg:#e3f5ec;--warn:#c97a06;--warn-bg:#fdf1dc;--bad:#d42a3c;--bad-bg:#fde6e8;
  --info:#2f6fe4;--hum:#2b8fd6;--temp:#f0742a;--shadow:0 1px 2px rgba(20,27,45,.06),0 4px 16px rgba(20,27,45,.06);
}
@media (prefers-color-scheme:dark){:root{
  --bg:#0c111c;--card:#151c2b;--ink:#e8ecf5;--muted:#93a0bb;--line:#253048;--track:#232d42;
  --ok:#3ccf8e;--ok-bg:#12301f;--warn:#f5b041;--warn-bg:#33270f;--bad:#ff5d6c;--bad-bg:#3a1519;
  --info:#6e9bff;--hum:#4fb3f5;--temp:#ff8c4a;--shadow:none;
}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:1080px;margin:0 auto;padding:20px 16px 32px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:16px}
h1{font-size:22px;margin:0;letter-spacing:-.01em}
.sub{color:var(--muted);font-size:13px;margin-top:2px}
.live{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--muted);background:var(--card);border:1px solid var(--line);border-radius:999px;padding:6px 12px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--muted)}
.live.on .dot{background:var(--ok);box-shadow:0 0 0 0 var(--ok);animation:pulse 2s infinite}
.live.off .dot{background:var(--bad)}
@keyframes pulse{0%{box-shadow:0 0 0 0 color-mix(in srgb,var(--ok) 60%,transparent)}70%{box-shadow:0 0 0 7px transparent}100%{box-shadow:0 0 0 0 transparent}}
.banner{display:flex;align-items:center;gap:12px;border-radius:14px;padding:14px 16px;margin-bottom:16px;background:var(--ok-bg);color:var(--ok);font-weight:600}
.banner .ico{flex:none;width:28px;height:28px;border-radius:50%;display:grid;place-items:center;background:currentColor}
.banner .ico svg{width:16px;height:16px;stroke:var(--card);fill:none;stroke-width:2.5;stroke-linecap:round;stroke-linejoin:round}
.banner small{display:block;font-weight:400;color:var(--ink);opacity:.8;font-size:13px}
.banner.warn{background:var(--warn-bg);color:var(--warn)}
.banner.bad{background:var(--bad-bg);color:var(--bad)}
.banner.bad.flash{animation:flash 1.2s ease-in-out infinite}
@keyframes flash{50%{filter:brightness(1.08) saturate(1.3)}}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;box-shadow:var(--shadow);min-width:0}
.c4{grid-column:span 4}.c8{grid-column:span 8}.c12{grid-column:span 12}
@media (max-width:860px){.c4{grid-column:span 6}.c8,.c12{grid-column:span 12}.feels{grid-column:span 12}}
@media (max-width:560px){.c4{grid-column:span 12}}
.label{font-size:13px;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;margin:0 0 6px}
.gauge{position:relative;max-width:260px;margin:4px auto 0}
.gauge svg{display:block;width:100%;height:auto}
.gauge .read{position:absolute;left:0;right:0;bottom:4px;text-align:center}
.big{font-size:40px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums;line-height:1}
.big .u{font-size:18px;font-weight:600;color:var(--muted);margin-left:2px}
.scale{display:flex;justify-content:space-between;color:var(--muted);font-size:12px;max-width:260px;margin:2px auto 0;padding:0 4px}
.note{text-align:center;color:var(--muted);font-size:13px;margin-top:8px}
.mk{display:inline-block;width:3px;height:12px;border-radius:2px;background:var(--bad);vertical-align:-1px;margin-right:7px;transform:rotate(30deg)}
.feels,.settings{display:flex;flex-direction:column}
.foot{margin:auto 0 0;padding-top:14px;font-size:12px}
.feels .big{font-size:52px;margin:18px 0 6px}
.tag{display:inline-block;padding:3px 10px;border-radius:999px;font-size:13px;font-weight:600;background:var(--track)}
.rows{margin-top:auto;padding-top:14px;display:grid;gap:8px;font-size:14px}
.rows div{display:flex;justify-content:space-between;border-top:1px solid var(--line);padding-top:8px}
.rows span:last-child{font-weight:600;font-variant-numeric:tabular-nums}
.chart-head{display:flex;justify-content:space-between;align-items:baseline;gap:8px;flex-wrap:wrap}
.legend{display:flex;gap:14px;font-size:13px;color:var(--muted);flex-wrap:wrap}
.legend i{display:inline-block;width:14px;height:3px;border-radius:2px;vertical-align:middle;margin-right:6px}
#chart{width:100%;height:220px;display:block;margin-top:8px}
#chart text{fill:var(--muted);font-size:11px}
.empty{fill:var(--muted);font-size:13px}
.ctl{display:flex;align-items:center;gap:8px;margin:10px 0 4px}
.step{display:flex;align-items:center;border:1px solid var(--line);border-radius:12px;overflow:hidden}
.step button{width:42px;height:44px;border:0;background:var(--track);color:var(--ink);font-size:20px;cursor:pointer}
.step input{width:76px;height:44px;border:0;background:var(--card);color:var(--ink);text-align:center;font:600 18px system-ui,sans-serif;-moz-appearance:textfield}
.step input::-webkit-inner-spin-button{-webkit-appearance:none}
.btn{height:44px;padding:0 16px;border-radius:12px;border:0;background:var(--info);color:#fff;font-weight:600;font-size:15px;cursor:pointer}
.btn:disabled{opacity:.4;cursor:default}
.hint{color:var(--muted);font-size:13px}
.switch{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:18px;padding-top:14px;border-top:1px solid var(--line);cursor:pointer}
.switch input{position:absolute;opacity:0}
.track{flex:none;width:52px;height:30px;border-radius:999px;background:var(--track);position:relative;transition:background .2s}
.track::after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3);transition:transform .2s}
.switch input:checked+.track{background:var(--warn)}
.switch input:checked+.track::after{transform:translateX(22px)}
.switch input:focus-visible+.track{outline:2px solid var(--info);outline-offset:2px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px;margin-right:6px;vertical-align:-2px}
.bars b{width:4px;border-radius:1px;background:var(--track)}
.bars b.on{background:var(--ok)}
.toast{position:fixed;left:50%;bottom:20px;transform:translate(-50%,30px);opacity:0;background:var(--ink);color:var(--card);padding:10px 16px;border-radius:10px;font-size:14px;transition:.25s;pointer-events:none}
.toast.show{opacity:1;transform:translate(-50%,0)}
footer{color:var(--muted);font-size:12px;text-align:center;margin-top:20px}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>Smart Room Monitor</h1>
      <div class="sub">Device <b id="dev">&ndash;</b> &middot; <span id="ip">&ndash;</span></div>
    </div>
    <div class="live" id="live"><span class="dot"></span><span id="liveText">Connecting&hellip;</span></div>
  </header>

  <div class="banner warn" id="banner">
    <span class="ico"><svg viewBox="0 0 24 24" id="bannerIco"></svg></span>
    <div><span id="bannerTitle">Connecting to the sensor&hellip;</span><small id="bannerText">Waiting for the first reading.</small></div>
  </div>

  <div class="grid">
    <section class="card c4">
      <p class="label">Temperature</p>
      <div class="gauge">
        <svg viewBox="0 0 200 112" aria-hidden="true">
          <path d="M20 100A80 80 0 0 1 180 100" pathLength="100" fill="none" stroke="var(--track)" stroke-width="14" stroke-linecap="round"/>
          <path id="tArc" d="M20 100A80 80 0 0 1 180 100" pathLength="100" fill="none" stroke="var(--temp)" stroke-width="14" stroke-linecap="round" stroke-dasharray="0 1000" style="transition:stroke-dasharray .6s,stroke .3s"/>
          <line id="tMark" x1="0" y1="0" x2="0" y2="0" stroke="var(--bad)" stroke-width="3" stroke-linecap="round"/>
        </svg>
        <div class="read"><span class="big" id="temp">&ndash;</span><span class="big"><span class="u">&deg;C</span></span></div>
      </div>
      <div class="scale"><span>10&deg;</span><span>40&deg;</span></div>
      <div class="note"><span class="mk"></span>Alert at <b id="thrNote">&ndash;</b></div>
    </section>

    <section class="card c4">
      <p class="label">Humidity</p>
      <div class="gauge">
        <svg viewBox="0 0 200 112" aria-hidden="true">
          <path d="M20 100A80 80 0 0 1 180 100" pathLength="100" fill="none" stroke="var(--track)" stroke-width="14" stroke-linecap="round"/>
          <path d="M20 100A80 80 0 0 1 180 100" pathLength="100" fill="none" stroke="var(--ok)" stroke-opacity=".28" stroke-width="14" stroke-dasharray="0 30 30 100"/>
          <path id="hArc" d="M20 100A80 80 0 0 1 180 100" pathLength="100" fill="none" stroke="var(--hum)" stroke-width="14" stroke-linecap="round" stroke-dasharray="0 1000" style="transition:stroke-dasharray .6s"/>
        </svg>
        <div class="read"><span class="big" id="hum">&ndash;</span><span class="big"><span class="u">%</span></span></div>
      </div>
      <div class="scale"><span>0%</span><span>100%</span></div>
      <div class="note"><span id="humNote">Comfort range 30&ndash;60%</span></div>
    </section>

    <section class="card c4 feels">
      <p class="label">Feels like</p>
      <div class="big" id="feels">&ndash;<span class="u">&deg;C</span></div>
      <div><span class="tag" id="feelsTag">&ndash;</span></div>
      <div class="rows">
        <div><span>Range since boot</span><span id="range">&ndash;</span></div>
        <div><span>Trend (last 10 min)</span><span id="trend">&ndash;</span></div>
        <div><span>Forecast in 10 min</span><span id="fcTemp">&ndash;</span></div>
        <div><span>Alert expected</span><span id="fcEta">&ndash;</span></div>
      </div>
    </section>

    <section class="card c8">
      <div class="chart-head">
        <p class="label">Last 20 minutes</p>
        <div class="legend"><span><i style="background:var(--temp)"></i>Temperature</span><span><i style="background:var(--hum)"></i>Humidity</span><span><i style="background:var(--bad);height:2px"></i>Alert threshold</span></div>
      </div>
      <svg id="chart" role="img" aria-label="Temperature and humidity over the last 20 minutes"></svg>
    </section>

    <section class="card c4 settings">
      <p class="label">Alert settings</p>
      <div class="hint">Buzzer sounds at or above this temperature.</div>
      <div class="ctl">
        <div class="step">
          <button type="button" id="minus" aria-label="Lower threshold">&minus;</button>
          <input id="thr" type="number" step="0.5" min="5" max="60" inputmode="decimal" aria-label="Alert threshold in degrees Celsius">
          <button type="button" id="plus" aria-label="Raise threshold">+</button>
        </div>
        <button class="btn" id="apply" disabled>Apply</button>
      </div>
      <label class="switch">
        <span><b>Mute buzzer</b><br><span class="hint">LCD and dashboard still show alerts</span></span>
        <input type="checkbox" id="mute"><span class="track"></span>
      </label>
      <p class="hint foot">Changes take effect on the board immediately and reach Grafana over MQTT.</p>
    </section>

    <section class="card c12">
      <p class="label">Device</p>
      <div class="rows" style="grid-template-columns:repeat(auto-fit,minmax(200px,1fr));column-gap:24px;padding-top:4px">
        <div><span>Wi-Fi signal</span><span id="rssi">&ndash;</span></div>
        <div><span>MQTT broker</span><span id="mqtt">&ndash;</span></div>
        <div><span>Uptime</span><span id="uptime">&ndash;</span></div>
        <div><span>Sensor read errors</span><span id="errs">&ndash;</span></div>
      </div>
    </section>
  </div>
  <footer>CMPE 286 &middot; NodeMCU ESP8266 + DHT11 &middot; updates every 2 s</footer>
</div>
<div class="toast" id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const T_MIN = 10, T_MAX = 40;
let status = null, history = null, lastOk = 0, thrDirty = false;

const ICONS = {
  ok: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  warn: '<path d="M12 7v6M12 17h.01"/>',
  bad: '<path d="M12 6v8M12 18h.01"/>'
};

function fmt(v, d = 1) { return v == null ? '–' : Number(v).toFixed(d); }
function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }
function pt(f, r) { const a = Math.PI * (1 - f); return [100 + r * Math.cos(a), 100 - r * Math.sin(a)]; }

function tempColor(t, thr) {
  if (t >= thr) return 'var(--bad)';
  if (t >= thr - 3) return 'var(--warn)';
  if (t < 18) return 'var(--info)';
  return 'var(--ok)';
}

function feelsLabel(t) {
  if (t < 18) return ['Cool', 'var(--info)'];
  if (t < 24) return ['Comfortable', 'var(--ok)'];
  if (t < 27) return ['Warm', 'var(--warn)'];
  if (t < 32) return ['Hot', 'var(--bad)'];
  return ['Very hot', 'var(--bad)'];
}

function setBanner(kind, title, text) {
  $('banner').className = 'banner ' + kind + (kind === 'bad' && status && status.alert && !status.muted ? ' flash' : '');
  $('bannerIco').innerHTML = ICONS[kind];
  $('bannerTitle').textContent = title;
  $('bannerText').textContent = text;
}

function setLive(on, text) {
  $('live').className = 'live ' + (on ? 'on' : 'off');
  $('liveText').textContent = text;
}

function renderStatus() {
  const s = status;
  $('dev').textContent = s.device;
  $('ip').textContent = s.ip;
  document.title = s.has_reading ? fmt(s.temperature) + '°C · Smart Room' : 'Smart Room Monitor';

  if (!s.has_reading) setBanner('warn', 'Waiting for the first reading', 'The sensor has not reported yet.');
  else if (s.sensor_error) setBanner('warn', 'Sensor error', 'The last DHT11 read failed. Showing the last good reading.');
  else if (s.alert) setBanner('bad', 'High temperature alert', s.muted
      ? 'Room is at or above ' + fmt(s.threshold) + '°C. Buzzer is muted.'
      : 'Room is at or above ' + fmt(s.threshold) + '°C. Buzzer is sounding.');
  else setBanner('ok', 'Temperature normal', 'Alert will trigger at ' + fmt(s.threshold) + '°C.');

  if (s.has_reading) {
    const t = s.temperature;
    $('temp').textContent = fmt(t);
    $('tArc').setAttribute('stroke-dasharray', (clamp((t - T_MIN) / (T_MAX - T_MIN), 0.01, 1) * 100) + ' 1000');
    $('tArc').setAttribute('stroke', tempColor(t, s.threshold));
    $('hum').textContent = fmt(s.humidity, 0);
    $('hArc').setAttribute('stroke-dasharray', clamp(s.humidity / 100, 0.01, 1) * 100 + ' 1000');
    $('humNote').textContent = s.humidity < 30 ? 'Dry · comfort range 30–60%' : s.humidity > 60 ? 'Humid · comfort range 30–60%' : 'Within comfort range 30–60%';
    const [lbl, col] = feelsLabel(s.heat_index);
    $('feels').innerHTML = fmt(s.heat_index) + '<span class="u">&deg;C</span>';
    $('feelsTag').textContent = lbl;
    $('feelsTag').style.color = col;
    $('range').textContent = fmt(s.min_temp) + ' – ' + fmt(s.max_temp) + '°C';
  }
  renderForecast(s);

  const [mx1, my1] = pt(clamp((s.threshold - T_MIN) / (T_MAX - T_MIN), 0, 1), 70);
  const [mx2, my2] = pt(clamp((s.threshold - T_MIN) / (T_MAX - T_MIN), 0, 1), 94);
  const m = $('tMark');
  m.setAttribute('x1', mx1); m.setAttribute('y1', my1); m.setAttribute('x2', mx2); m.setAttribute('y2', my2);
  $('thrNote').textContent = fmt(s.threshold) + '°C';

  if (!thrDirty) $('thr').value = fmt(s.threshold);
  $('apply').disabled = !thrDirty;
  $('mute').checked = s.muted;

  const r = s.rssi, n = r > -55 ? 4 : r > -67 ? 3 : r > -75 ? 2 : r > -85 ? 1 : 0;
  $('rssi').innerHTML = '<span class="bars">' + [6, 9, 12, 14].map((h, i) => '<b style="height:' + h + 'px" class="' + (i < n ? 'on' : '') + '"></b>').join('') + '</span>' + r + ' dBm';
  $('mqtt').innerHTML = s.mqtt ? '<span style="color:var(--ok)">Connected</span>' : '<span style="color:var(--bad)">Disconnected</span>';
  const u = s.uptime_s, d = Math.floor(u / 86400), h = Math.floor(u % 86400 / 3600), mi = Math.floor(u % 3600 / 60);
  $('uptime').textContent = (d ? d + 'd ' : '') + (d || h ? h + 'h ' : '') + mi + 'm';
  $('errs').textContent = s.sensor_errors;
}

// From server/forecaster over MQTT; null when the forecaster isn't running yet.
function renderForecast(s) {
  const eta = $('fcEta');
  eta.style.color = '';
  if (s.forecast_temp == null) {
    $('fcTemp').textContent = '–';
    eta.textContent = 'No forecast yet';
    eta.style.color = 'var(--muted)';
    return;
  }
  $('fcTemp').textContent = fmt(s.forecast_temp) + '°C';
  const m = s.forecast_minutes;
  if (s.alert || m === 0) { eta.textContent = 'Now'; eta.style.color = 'var(--bad)'; }
  else if (m == null) { eta.textContent = 'Not soon'; eta.style.color = 'var(--ok)'; }
  else { eta.textContent = m < 1 ? 'In < 1 min' : 'In ~' + Math.round(m) + ' min'; eta.style.color = 'var(--warn)'; }
}

function renderTrend() {
  if (!history || history.temperature.length < 2) { $('trend').textContent = '–'; return; }
  const a = history.temperature.filter(v => v != null);
  const back = Math.min(a.length - 1, Math.round(600 / history.interval_s));
  const diff = a[a.length - 1] - a[a.length - 1 - back];
  const arrow = Math.abs(diff) < 0.3 ? '→ steady' : diff > 0 ? '↗ +' + fmt(diff) + '°C' : '↘ ' + fmt(diff) + '°C';
  $('trend').textContent = arrow;
}

function renderChart() {
  const svg = $('chart'), W = svg.clientWidth || 600, H = svg.clientHeight || 220;
  svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
  if (!history || history.temperature.length < 2) {
    svg.innerHTML = '<text class="empty" x="' + W / 2 + '" y="' + H / 2 + '" text-anchor="middle">Collecting data &mdash; the chart fills in over the next few minutes</text>';
    return;
  }
  const L = 34, R = 40, Tp = 10, B = 24, iw = W - L - R, ih = H - Tp - B;
  const temps = history.temperature, hums = history.humidity, n = temps.length, step = history.interval_s;
  const thr = status ? status.threshold : null;
  const vals = temps.filter(v => v != null).concat(thr != null ? [thr] : []);
  let lo = Math.floor(Math.min(...vals) - 1), hi = Math.ceil(Math.max(...vals) + 1);
  if (hi - lo < 8) { const c = Math.round((hi + lo) / 2); lo = c - 4; hi = c + 4; }
  hi = lo + Math.ceil((hi - lo) / 4) * 4;
  const slots = Math.max(n, 120);
  const x = i => L + iw * (slots - n + i) / (slots - 1);
  const yT = v => Tp + ih * (1 - (v - lo) / (hi - lo));
  const yH = v => Tp + ih * (1 - v / 100);
  const line = (arr, y) => arr.map((v, i) => v == null ? '' : (i && arr[i - 1] != null ? 'L' : 'M') + x(i).toFixed(1) + ' ' + y(v).toFixed(1)).join('');
  let g = '';
  for (let k = 0; k <= 4; k++) {
    const v = lo + (hi - lo) * k / 4, yy = yT(v);
    g += '<line x1="' + L + '" x2="' + (W - R) + '" y1="' + yy + '" y2="' + yy + '" stroke="var(--line)"/>';
    g += '<text x="' + (L - 6) + '" y="' + (yy + 4) + '" text-anchor="end">' + v.toFixed(0) + '°</text>';
    g += '<text x="' + (W - R + 6) + '" y="' + (yy + 4) + '">' + (100 * k / 4).toFixed(0) + '%</text>';
  }
  for (let mAgo = 20; mAgo >= 0; mAgo -= 5) {
    const i = slots - 1 - mAgo * 60 / step, xx = L + iw * i / (slots - 1);
    g += '<text x="' + xx + '" y="' + (H - 6) + '" text-anchor="middle">' + (mAgo ? '-' + mAgo + 'm' : 'now') + '</text>';
  }
  const first = temps.findIndex(v => v != null);
  const tPath = line(temps, yT);
  g += '<defs><linearGradient id="tg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="var(--temp)" stop-opacity=".28"/><stop offset="1" stop-color="var(--temp)" stop-opacity="0"/></linearGradient></defs>';
  g += '<path d="' + tPath + 'L' + x(n - 1).toFixed(1) + ' ' + (Tp + ih) + 'L' + x(first).toFixed(1) + ' ' + (Tp + ih) + 'Z" fill="url(#tg)"/>';
  g += '<path d="' + line(hums, yH) + '" fill="none" stroke="var(--hum)" stroke-width="2" stroke-opacity=".8"/>';
  if (thr != null && thr >= lo && thr <= hi) g += '<line x1="' + L + '" x2="' + (W - R) + '" y1="' + yT(thr) + '" y2="' + yT(thr) + '" stroke="var(--bad)" stroke-width="1.5" stroke-dasharray="6 5"/>';
  g += '<path d="' + tPath + '" fill="none" stroke="var(--temp)" stroke-width="2.5" stroke-linejoin="round"/>';
  const lv = temps[n - 1];
  if (lv != null) g += '<circle cx="' + x(n - 1) + '" cy="' + yT(lv) + '" r="4" fill="var(--temp)" stroke="var(--card)" stroke-width="2"/>';
  svg.innerHTML = g;
}

async function getJSON(url, opts) {
  const ctl = new AbortController(), to = setTimeout(() => ctl.abort(), 4000);
  try {
    const r = await fetch(url, Object.assign({ cache: 'no-store', signal: ctl.signal }, opts));
    if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.statusText);
    return await r.json();
  } finally { clearTimeout(to); }
}

async function pollStatus() {
  try {
    status = await getJSON('/api/status');
    lastOk = Date.now();
    setLive(true, 'Live');
    renderStatus();
  } catch (e) {
    if (Date.now() - lastOk > 6000) {
      setLive(false, 'Offline');
      setBanner('warn', 'Lost connection to the board', 'Check that the NodeMCU is powered and on the same Wi-Fi network.');
    }
  }
}

async function pollHistory() {
  try { history = await getJSON('/api/history'); renderChart(); renderTrend(); } catch (e) {}
}

function toast(msg) {
  const t = $('toast'); t.textContent = msg; t.classList.add('show');
  clearTimeout(toast.h); toast.h = setTimeout(() => t.classList.remove('show'), 2200);
}

async function post(url, value) {
  return getJSON(url, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'value=' + encodeURIComponent(value) });
}

function nudge(d) {
  const v = clamp((parseFloat($('thr').value) || 30) + d, 5, 60);
  $('thr').value = v.toFixed(1); thrDirty = true; $('apply').disabled = false;
}
$('minus').onclick = () => nudge(-0.5);
$('plus').onclick = () => nudge(0.5);
$('thr').oninput = () => { thrDirty = true; $('apply').disabled = false; };
$('thr').onkeydown = e => { if (e.key === 'Enter') $('apply').click(); };
$('apply').onclick = async () => {
  const v = parseFloat($('thr').value);
  if (!(v >= 5 && v <= 60)) { toast('Enter a value between 5 and 60 °C'); return; }
  $('apply').disabled = true;
  try { status = await post('/api/threshold', v); thrDirty = false; renderStatus(); renderChart(); toast('Alert threshold set to ' + fmt(v) + ' °C'); }
  catch (e) { $('apply').disabled = false; toast('Could not update: ' + e.message); }
};
$('mute').onchange = async e => {
  const on = e.target.checked;
  try { status = await post('/api/mute', on ? 1 : 0); renderStatus(); toast(on ? 'Buzzer muted' : 'Buzzer unmuted'); }
  catch (err) { e.target.checked = !on; toast('Could not update: ' + err.message); }
};

window.addEventListener('resize', renderChart);
pollStatus(); pollHistory();
setInterval(pollStatus, 2000);
setInterval(pollHistory, 10000);
</script>
</body>
</html>
)rawliteral";
