'use strict';

// ─── Tab Navigation ──────────────────────────────────────────────────────────

document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn, .tab-pane').forEach(el => el.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById(btn.dataset.tab).classList.add('active');
    if (btn.dataset.tab === 'tab-live') startLivePolling();
    else stopLivePolling();
    if (btn.dataset.tab === 'tab-faces')    { loadFaces(); loadImageStatus(); }
    if (btn.dataset.tab === 'tab-warnings') loadWarnings();
  });
});

// ─── Toast ───────────────────────────────────────────────────────────────────

function toast(msg, type = 'ok') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = `show ${type}`;
  setTimeout(() => el.classList.remove('show'), 3000);
}

// ─── Load Config ─────────────────────────────────────────────────────────────

async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    const cfg = await r.json();

    setVal('protocol',       cfg.protocol);
    setVal('can_speed',      cfg.can_speed);
    setVal('can_tx_pin',     cfg.can_tx_pin);
    setVal('can_rx_pin',     cfg.can_rx_pin);
    setVal('link_base_id',   '0x' + cfg.link_base_id.toString(16).toUpperCase());
    setVal('haltech_base_id','0x' + cfg.haltech_base_id.toString(16).toUpperCase());
    setVal('wifi_ssid',      cfg.wifi_ssid);
    setVal('rpm_redline',    cfg.rpm_redline);
    setVal('coolant_warn',   cfg.coolant_warn_c);
    setVal('oil_press_warn', cfg.oil_press_warn);

    if (cfg.colors) {
      setColorVal('color_normal',  cfg.colors.normal);
      setColorVal('color_warn',    cfg.colors.warn);
      setColorVal('color_danger',  cfg.colors.danger);
      setColorVal('color_text',    cfg.colors.text);
      setColorVal('color_arc_bg',  cfg.colors.arc_bg);
    }

    setCheck('display_temp_f', cfg.display_temp_f);
    setCheck('display_psi',    cfg.display_psi);
    setCheck('display_mph',    cfg.display_mph);
    setCheck('display_afr',    cfg.display_afr);

    updateProtocolVisibility(cfg.protocol);
  } catch (e) {
    toast('Failed to load config', 'err');
  }
}

function setVal(id, v) {
  const el = document.getElementById(id);
  if (el) el.value = v;
}

function setCheck(id, v) {
  const el = document.getElementById(id);
  if (el) el.checked = !!v;
}

// Color pickers use "#RRGGBB" format; API sends "RRGGBB"
function setColorVal(id, hex6) {
  const el = document.getElementById(id);
  if (el) el.value = '#' + hex6;
}

function getColorVal(id) {
  const el = document.getElementById(id);
  if (!el) return '000000';
  return el.value.replace('#', '');
}

function updateProtocolVisibility(proto) {
  document.getElementById('link-opts').style.display    = (proto == 0) ? '' : 'none';
  document.getElementById('haltech-opts').style.display = (proto == 1) ? '' : 'none';
}

document.getElementById('protocol').addEventListener('change', function () {
  updateProtocolVisibility(parseInt(this.value));
});

// ─── Save Config ─────────────────────────────────────────────────────────────

document.getElementById('save-btn').addEventListener('click', async () => {
  const parseHex = id => parseInt(document.getElementById(id).value, 16);

  const body = {
    protocol:        parseInt(document.getElementById('protocol').value),
    can_speed:       parseInt(document.getElementById('can_speed').value),
    can_tx_pin:      parseInt(document.getElementById('can_tx_pin').value),
    can_rx_pin:      parseInt(document.getElementById('can_rx_pin').value),
    link_base_id:    parseHex('link_base_id'),
    haltech_base_id: parseHex('haltech_base_id'),
    wifi_ssid:       document.getElementById('wifi_ssid').value,
    wifi_password:   document.getElementById('wifi_password').value,
    rpm_redline:     parseInt(document.getElementById('rpm_redline').value),
    coolant_warn_c:  parseFloat(document.getElementById('coolant_warn').value),
    oil_press_warn:  parseFloat(document.getElementById('oil_press_warn').value),
    colors: {
      normal:  getColorVal('color_normal'),
      warn:    getColorVal('color_warn'),
      danger:  getColorVal('color_danger'),
      text:    getColorVal('color_text'),
      arc_bg:  getColorVal('color_arc_bg'),
    },
    display_temp_f: document.getElementById('display_temp_f').checked,
    display_psi:    document.getElementById('display_psi').checked,
    display_mph:    document.getElementById('display_mph').checked,
    display_afr:    document.getElementById('display_afr').checked,
  };

  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const res = await r.json();
    if (res.status === 'ok') {
      toast('Config saved — CAN bus restarted', 'ok');
    } else {
      toast('Save failed', 'err');
    }
  } catch (e) {
    toast('Network error: ' + e.message, 'err');
  }
});

document.getElementById('reset-btn').addEventListener('click', async () => {
  if (!confirm('Reset to factory defaults?')) return;
  try {
    await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ _reset: true }),
    });
    toast('Reset — reloading...', 'ok');
    setTimeout(loadConfig, 500);
  } catch (e) {
    toast('Reset failed', 'err');
  }
});

// ─── Live Data Polling ───────────────────────────────────────────────────────

let liveTimer = null;

function startLivePolling() {
  if (liveTimer) return;
  pollLive();
  liveTimer = setInterval(pollLive, 500);
}

function stopLivePolling() {
  if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
}

async function pollLive() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    updateLive(d);
  } catch (_) {}
}

function updateLive(d) {
  // Helper: set a data-cell's value with optional threshold colouring.
  // warnDir: 'hi' = warn when val >= warn, 'lo' = warn when val < warn
  const set = (id, val, warnLo, warnHi, dangerLo, dangerHi) => {
    const cell = document.getElementById(id);
    if (!cell) return;
    const n = parseFloat(val);
    cell.querySelector('.val').textContent = d.valid ? val : '--';
    cell.classList.remove('warn', 'danger');
    if (!d.valid || isNaN(n)) return;
    if ((dangerHi !== undefined && n >= dangerHi) ||
        (dangerLo !== undefined && n < dangerLo))   { cell.classList.add('danger'); return; }
    if ((warnHi  !== undefined && n >= warnHi)  ||
        (warnLo  !== undefined && n < warnLo))       { cell.classList.add('warn'); }
  };

  // ── Engine ──────────────────────────────────────────────────────────────────
  set('live-rpm',      d.rpm.toFixed(0),            undefined, 6000, undefined, 7000);
  set('live-map',      d.map_kpa.toFixed(1));
  set('live-tps',      d.tps_pct.toFixed(1));
  set('live-inj-pri',  d.inj_duty_pri.toFixed(1),   undefined, 80,   undefined, 95);
  set('live-inj-sec',  d.inj_duty_sec.toFixed(1),   undefined, 80,   undefined, 95);
  set('live-ign',      d.ign_angle.toFixed(1));
  set('live-lambda1',  d.lambda1.toFixed(3));
  set('live-lambda2',  d.lambda2.toFixed(3));
  set('live-t-boost',  d.target_boost.toFixed(1));

  // ── Pressures & Electrical ──────────────────────────────────────────────────
  set('live-oil-p',    d.oil_press_bar.toFixed(2),  2.0, undefined, 1.5, undefined);
  set('live-fuel-p',   d.fuel_press_bar.toFixed(2));
  set('live-bat',      d.battery_v.toFixed(2),      12.0, undefined, 11.5, undefined);
  set('live-baro',     d.baro_kpa.toFixed(1));

  // ── Temperatures ────────────────────────────────────────────────────────────
  set('live-coolant',  d.coolant_c.toFixed(1),      undefined, 95,  undefined, 105);
  set('live-oil-t',    d.oil_temp_c.toFixed(1),     undefined, 120, undefined, 135);
  set('live-iat',      d.iat_c.toFixed(1));
  set('live-fuel-t',   d.fuel_temp_c.toFixed(1),    undefined, 60,  undefined, 75);

  // ── Vehicle ─────────────────────────────────────────────────────────────────
  set('live-speed',    d.speed_kph.toFixed(0));
  set('live-gear',     d.gear >= 0 ? d.gear : '--');
  set('live-cam1',     d.intake_cam1.toFixed(1));
  set('live-cam2',     d.intake_cam2.toFixed(1));
  set('live-fuel-con', d.fuel_consump.toFixed(1));
  set('live-fuel-eco', d.fuel_economy.toFixed(1));

  // ── EGTs ────────────────────────────────────────────────────────────────────
  if (Array.isArray(d.egt)) {
    for (let i = 0; i < 12; i++) {
      set(`live-egt${i + 1}`, d.egt[i].toFixed(0), undefined, 900, undefined, 1000);
    }
  }

  // ── Diagnostics ─────────────────────────────────────────────────────────────
  set('live-miss',    d.miss_counter,    undefined, undefined, undefined, 0);
  set('live-trigger', d.trigger_counter);

  const status = document.getElementById('live-status');
  if (status) {
    status.textContent = d.valid ? 'Receiving CAN data' : 'No CAN data';
    status.style.color = d.valid ? 'var(--accent)' : 'var(--danger)';
  }
}

// ─── Init ────────────────────────────────────────────────────────────────────

loadConfig();

// ─── Gauge Faces ─────────────────────────────────────────────────────────────

const FIELD_OPTIONS = [
  // Core (all protocols)
  [ 0, 'None'],
  [ 1, 'RPM'],
  [ 2, 'Coolant \xB0C'],
  [ 3, 'Oil Temp \xB0C'],
  [ 4, 'Oil Press bar'],
  [ 5, 'TPS %'],
  [ 6, 'MAP kPa'],
  [ 7, 'Lambda 1'],
  [ 8, 'Battery V'],
  [ 9, 'Speed km/h'],
  // Haltech extended
  [10, 'Fuel Press bar'],
  [11, 'IAT \xB0C'],
  [12, 'Lambda 2'],
  [13, 'Inj Duty Pri %'],
  [14, 'Inj Duty Sec %'],
  [15, 'Ign Angle \xB0'],
  [16, 'Gear'],
  [17, 'Intake Cam 1 \xB0'],
  [18, 'Intake Cam 2 \xB0'],
  [19, 'Target Boost kPa'],
  [20, 'Baro kPa'],
  [21, 'EGT 1 \xB0C'],
  [22, 'EGT 2 \xB0C'],
  [23, 'EGT 3 \xB0C'],
  [24, 'EGT 4 \xB0C'],
  [25, 'EGT 5 \xB0C'],
  [26, 'EGT 6 \xB0C'],
  [27, 'EGT 7 \xB0C'],
  [28, 'EGT 8 \xB0C'],
  [29, 'EGT 9 \xB0C'],
  [30, 'EGT 10 \xB0C'],
  [31, 'EGT 11 \xB0C'],
  [32, 'EGT 12 \xB0C'],
  [33, 'Fuel Temp \xB0C'],
  [34, 'Fuel Consump L/hr'],
  [35, 'Avg Economy L'],
];

const LAYOUT_NAMES = ['RPM Arc', 'Single', 'Dual', 'Quad (2\xD72)', 'Dial'];

// How many visible slot selectors each layout needs
const SLOT_LABELS = {
  0: ['Cell 1', 'Cell 2'],   // RPM arc has 2 configurable info cells
  1: ['Field'],
  2: ['Top', 'Bottom'],
  3: ['Top-Left', 'Top-Right', 'Bottom-Left', 'Bottom-Right'],
  4: ['Field'],
};

let s_faces = null;

async function loadFaces() {
  try {
    const r = await fetch('/api/faces');
    s_faces = (await r.json()).faces;
    renderFaceEditor(s_faces);
  } catch (e) {
    toast('Failed to load face config', 'err');
  }
}

function fieldOptHtml(selected) {
  return FIELD_OPTIONS.map(([v, n]) =>
    `<option value="${v}"${v === selected ? ' selected' : ''}>${n}</option>`
  ).join('');
}

function renderSlots(faceIdx, layout, slots) {
  const labels = SLOT_LABELS[layout] || SLOT_LABELS[0];
  return labels.map((label, s) => `
    <div class="slot-row">
      <label>${label}</label>
      <select data-face="${faceIdx}" data-slot="${s}" onchange="onSlotChange(this)">
        ${fieldOptHtml(slots[s] !== undefined ? slots[s] : 0)}
      </select>
    </div>`
  ).join('');
}

// Dial config fields (shown only for FACE_LAYOUT_DIAL = 4)
function renderDialConfig(faceIdx, face) {
  if (face.layout !== 4) return '';
  const dm    = face.dial_min       !== undefined ? face.dial_min       : 0;
  const dM    = face.dial_max       !== undefined ? face.dial_max       : 300;
  const dw    = face.dial_warn      !== undefined ? face.dial_warn      : 200;
  const dph   = face.dial_peak_hold !== undefined ? face.dial_peak_hold : 3000;
  return `
    <div class="dial-config">
      <h3 style="margin:0.8rem 0 0.4rem;font-size:0.9rem;color:var(--muted)">Dial Settings</h3>
      <div class="form-grid">
        <div class="field">
          <label>Scale Min</label>
          <input type="number" step="any" id="dial_min_${faceIdx}" value="${dm}"
                 onchange="onDialChange(${faceIdx})">
        </div>
        <div class="field">
          <label>Scale Max</label>
          <input type="number" step="any" id="dial_max_${faceIdx}" value="${dM}"
                 onchange="onDialChange(${faceIdx})">
        </div>
        <div class="field">
          <label>Peak Trigger (threshold)</label>
          <input type="number" step="any" id="dial_warn_${faceIdx}" value="${dw}"
                 onchange="onDialChange(${faceIdx})">
        </div>
        <div class="field">
          <label>Peak Hold (ms, 0 = hold until re-crossed)</label>
          <input type="number" step="100" min="0" id="dial_peak_hold_${faceIdx}" value="${dph}"
                 onchange="onDialChange(${faceIdx})">
        </div>
      </div>
    </div>`;
}

function onDialChange(i) {
  if (!s_faces) return;
  s_faces[i].dial_min       = parseFloat(document.getElementById(`dial_min_${i}`).value) || 0;
  s_faces[i].dial_max       = parseFloat(document.getElementById(`dial_max_${i}`).value) || 100;
  s_faces[i].dial_warn      = parseFloat(document.getElementById(`dial_warn_${i}`).value) || 0;
  s_faces[i].dial_peak_hold = parseInt(document.getElementById(`dial_peak_hold_${i}`).value) || 0;
  updateFacePreview(i);
}

function escSvg(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function fname(id) {
  const opt = FIELD_OPTIONS.find(([v]) => v === id);
  return opt ? opt[1] : 'None';
}

// Convert "RRGGBB" hex string to SVG colour
function svgCol(hex6) { return '#' + (hex6 || '00b4d8'); }

function buildSvgPreview(faceIdx, face) {
  const CX = 233, CY = 233, R = 225;
  const clipId = `clip${faceIdx}`;
  let content = '';

  if (face.layout === 0) {
    content += `<path d="M 81 385 A 215 215 0 1 1 385 385" fill="none" stroke="#1E1E1E" stroke-width="18" stroke-linecap="round"/>`;
    content += `<path d="M 81 385 A 215 215 0 0 1 233 18" fill="none" stroke="#00b4d8" stroke-width="18" stroke-linecap="round"/>`;
    content += `<text x="${CX}" y="210" text-anchor="middle" fill="#e0e0e0" font-size="30" font-weight="bold" font-family="sans-serif">RPM</text>`;
    content += `<text x="${CX}" y="246" text-anchor="middle" fill="#666" font-size="16" font-family="sans-serif">arc</text>`;
    content += `<text x="${CX}" y="75" text-anchor="middle" fill="#00b4d8" font-size="14" font-family="sans-serif">status</text>`;
    const cx = [116, 350];
    const cy = [330, 330];
    for (let i = 0; i < 2; i++) {
      const n = fname(face.slots[i] || 0);
      content += `<text x="${cx[i]}" y="${cy[i]}" text-anchor="middle" fill="#666" font-size="11" font-family="sans-serif">${escSvg(n)}</text>`;
      content += `<text x="${cx[i]}" y="${cy[i] + 17}" text-anchor="middle" fill="#e0e0e0" font-size="16" font-family="sans-serif">--</text>`;
    }
  } else if (face.layout === 1) {
    content += `<circle cx="${CX}" cy="${CY}" r="209" fill="none" stroke="#00b4d8" stroke-width="4" opacity="0.25"/>`;
    content += `<text x="${CX}" y="${CY - 8}" text-anchor="middle" fill="#666" font-size="20" font-family="sans-serif">${escSvg(fname(face.slots[0] || 0))}</text>`;
    content += `<text x="${CX}" y="${CY + 42}" text-anchor="middle" fill="#e0e0e0" font-size="42" font-weight="bold" font-family="sans-serif">--</text>`;
    content += `<text x="${CX}" y="75" text-anchor="middle" fill="#00b4d8" font-size="14" font-family="sans-serif">status</text>`;
  } else if (face.layout === 2) {
    content += `<circle cx="${CX}" cy="${CY}" r="209" fill="none" stroke="#00b4d8" stroke-width="4" opacity="0.25"/>`;
    content += `<line x1="${CX - 160}" y1="${CY}" x2="${CX + 160}" y2="${CY}" stroke="#2A2A2A" stroke-width="2"/>`;
    content += `<text x="${CX}" y="148" text-anchor="middle" fill="#666" font-size="16" font-family="sans-serif">${escSvg(fname(face.slots[0] || 0))}</text>`;
    content += `<text x="${CX}" y="194" text-anchor="middle" fill="#e0e0e0" font-size="38" font-weight="bold" font-family="sans-serif">--</text>`;
    content += `<text x="${CX}" y="272" text-anchor="middle" fill="#666" font-size="16" font-family="sans-serif">${escSvg(fname(face.slots[1] || 0))}</text>`;
    content += `<text x="${CX}" y="318" text-anchor="middle" fill="#e0e0e0" font-size="38" font-weight="bold" font-family="sans-serif">--</text>`;
    content += `<text x="${CX}" y="75" text-anchor="middle" fill="#00b4d8" font-size="14" font-family="sans-serif">status</text>`;
  } else if (face.layout === 3) {
    content += `<circle cx="${CX}" cy="${CY}" r="209" fill="none" stroke="#00b4d8" stroke-width="4" opacity="0.25"/>`;
    const qx = [130, 336, 130, 336];
    const qy = [148, 148, 293, 293];
    for (let i = 0; i < 4; i++) {
      content += `<text x="${qx[i]}" y="${qy[i]}" text-anchor="middle" fill="#666" font-size="14" font-family="sans-serif">${escSvg(fname(face.slots[i] || 0))}</text>`;
      content += `<text x="${qx[i]}" y="${qy[i] + 24}" text-anchor="middle" fill="#e0e0e0" font-size="24" font-weight="bold" font-family="sans-serif">--</text>`;
    }
    content += `<text x="${CX}" y="75" text-anchor="middle" fill="#00b4d8" font-size="14" font-family="sans-serif">status</text>`;
  } else if (face.layout === 4) {
    // ── Dial preview ─────────────────────────────────────────────────────────
    // Scale track
    content += `<path d="M 81 385 A 215 215 0 1 1 385 385" fill="none" stroke="#1E1E1E" stroke-width="14" stroke-linecap="round"/>`;
    // Warning zone (last ~25% of arc in red) — approximate, fixed for preview
    content += `<path d="M 385 385 A 215 215 0 0 0 385 81" fill="none" stroke="#FF3333" stroke-width="14" stroke-linecap="round" opacity="0.7"/>`;
    // Indicator arc at ~40% value (for illustration)
    content += `<path d="M 81 385 A 215 215 0 0 1 183 60" fill="none" stroke="#00b4d8" stroke-width="14" stroke-linecap="round"/>`;
    // Peak hold tick (white, near warning zone start)
    content += `<path d="M 370 112 A 215 215 0 0 0 350 95" fill="none" stroke="#ffffff" stroke-width="14" stroke-linecap="round"/>`;
    // Needle at ~40%
    content += `<line x1="257" y1="252" x2="178" y2="66" stroke="#ffffff" stroke-width="3" stroke-linecap="round"/>`;
    // Pivot circle
    content += `<circle cx="233" cy="233" r="8" fill="#ffffff"/>`;
    // Field label
    content += `<text x="${CX}" y="340" text-anchor="middle" fill="#e0e0e0" font-size="28" font-weight="bold" font-family="sans-serif">--</text>`;
    content += `<text x="${CX}" y="368" text-anchor="middle" fill="#666" font-size="16" font-family="sans-serif">${escSvg(fname(face.slots[0] || 0))}</text>`;
    content += `<text x="${CX}" y="396" text-anchor="middle" fill="#ff3333" font-size="13" font-family="sans-serif">PK --</text>`;
    content += `<text x="${CX}" y="75" text-anchor="middle" fill="#00b4d8" font-size="14" font-family="sans-serif">status</text>`;
  }

  return `<svg viewBox="0 0 466 466" xmlns="http://www.w3.org/2000/svg">
    <defs><clipPath id="${clipId}"><circle cx="${CX}" cy="${CY}" r="${R}"/></clipPath></defs>
    <circle cx="${CX}" cy="${CY}" r="${R}" fill="#0d0d0d"/>
    <g clip-path="url(#${clipId})">${content}</g>
    <circle cx="${CX}" cy="${CY}" r="${R}" fill="none" stroke="#2e2e2e" stroke-width="3"/>
  </svg>`;
}

function renderFaceEditor(faces) {
  const container = document.getElementById('faces-container');
  if (!container) return;
  container.innerHTML = faces.map((face, i) => `
    <div class="card face-card">
      <div class="face-header">
        <h2>Face ${i + 1}</h2>
        <label class="toggle">
          <input type="checkbox" id="face-enabled-${i}" ${face.enabled ? 'checked' : ''}
                 onchange="onFaceEnabledChange(${i})">
          <span>Enabled</span>
        </label>
      </div>
      <div class="face-body" id="face-body-${i}" ${face.enabled ? '' : 'style="opacity:0.45;pointer-events:none"'}>
        <div class="face-layout-row">
          <div class="field">
            <label>Layout</label>
            <select id="face-layout-${i}" onchange="onLayoutChange(${i})">
              ${LAYOUT_NAMES.map((n, v) => `<option value="${v}"${v === face.layout ? ' selected' : ''}>${n}</option>`).join('')}
            </select>
          </div>
        </div>
        <div class="face-slots-preview">
          <div class="slots-col" id="slots-${i}">
            ${renderSlots(i, face.layout, face.slots)}
            ${renderDialConfig(i, face)}
          </div>
          <div class="preview-col">
            <div class="face-preview" id="preview-${i}">
              ${buildSvgPreview(i, face)}
            </div>
          </div>
        </div>
      </div>
    </div>`
  ).join('');
}

function onFaceEnabledChange(i) {
  const enabled = document.getElementById(`face-enabled-${i}`).checked;
  const body    = document.getElementById(`face-body-${i}`);
  body.style.opacity       = enabled ? '1' : '0.45';
  body.style.pointerEvents = enabled ? '' : 'none';
  if (s_faces) s_faces[i].enabled = enabled ? 1 : 0;
}

function onLayoutChange(i) {
  const layout    = parseInt(document.getElementById(`face-layout-${i}`).value);
  const prevSlots = s_faces ? s_faces[i].slots.slice() : new Array(6).fill(0);
  if (s_faces) s_faces[i].layout = layout;
  const slotsEl = document.getElementById(`slots-${i}`);
  slotsEl.innerHTML = renderSlots(i, layout, prevSlots) + renderDialConfig(i, s_faces[i]);
  updateFacePreview(i);
}

function onSlotChange(el) {
  const i = parseInt(el.dataset.face);
  const s = parseInt(el.dataset.slot);
  if (s_faces) s_faces[i].slots[s] = parseInt(el.value);
  updateFacePreview(i);
}

function updateFacePreview(i) {
  if (!s_faces) return;
  const el = document.getElementById(`preview-${i}`);
  if (el) el.innerHTML = buildSvgPreview(i, s_faces[i]);
}

function collectFaces() {
  return Array.from({ length: 5 }, (_, i) => {
    const layout = parseInt(document.getElementById(`face-layout-${i}`).value);
    const labels = SLOT_LABELS[layout] || [];
    const slots  = new Array(6).fill(0);
    labels.forEach((_, s) => {
      const el = document.querySelector(`select[data-face="${i}"][data-slot="${s}"]`);
      if (el) slots[s] = parseInt(el.value);
    });

    const face = {
      enabled: document.getElementById(`face-enabled-${i}`).checked ? 1 : 0,
      layout,
      slots,
    };

    // Include dial config regardless of layout so values are preserved
    if (s_faces && s_faces[i]) {
      face.dial_min       = s_faces[i].dial_min       || 0;
      face.dial_max       = s_faces[i].dial_max       || 100;
      face.dial_warn      = s_faces[i].dial_warn      || 0;
      face.dial_peak_hold = s_faces[i].dial_peak_hold || 0;
    }

    return face;
  });
}

document.getElementById('save-faces-btn').addEventListener('click', async () => {
  try {
    const r = await fetch('/api/faces', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ faces: collectFaces() }),
    });
    const res = await r.json();
    if (res.status === 'ok') {
      toast('Faces saved \u2014 gauge updated', 'ok');
    } else {
      toast('Save failed', 'err');
    }
  } catch (e) {
    toast('Network error: ' + e.message, 'err');
  }
});

// ─── Custom Dial Image Management ────────────────────────────────────────────

async function loadImageStatus() {
  try {
    const r = await fetch('/api/images');
    const d = await r.json();
    setImgStatus('img-bg-status', d.has_background);
    setImgStatus('img-nd-status', d.has_needle);
  } catch (_) {}
}

function setImgStatus(id, present) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = present ? 'Stored on device' : 'Not uploaded';
  el.style.color = present ? 'var(--accent)' : 'var(--muted)';
}

async function uploadImage(fileInputId, endpoint, statusId) {
  const input = document.getElementById(fileInputId);
  if (!input || !input.files || !input.files[0]) {
    toast('Select a PNG file first', 'err');
    return;
  }
  const file = input.files[0];
  if (file.size > 512 * 1024) {
    toast('File too large (max 512 KB)', 'err');
    return;
  }
  try {
    const r = await fetch(endpoint, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: file,
    });
    const res = await r.json();
    if (res.status === 'ok') {
      toast('Image uploaded \u2014 dial face reloading', 'ok');
      setImgStatus(statusId, true);
      input.value = '';
    } else {
      toast('Upload failed', 'err');
    }
  } catch (e) {
    toast('Network error: ' + e.message, 'err');
  }
}

async function deleteImage(endpoint, statusId) {
  if (!confirm('Delete this image from the device?')) return;
  try {
    const r = await fetch(endpoint, { method: 'DELETE' });
    const res = await r.json();
    if (res.status === 'ok') {
      toast('Image deleted \u2014 dial face reloading', 'ok');
      setImgStatus(statusId, false);
    } else {
      toast('Delete failed', 'err');
    }
  } catch (e) {
    toast('Network error: ' + e.message, 'err');
  }
}

document.getElementById('img-bg-upload').addEventListener('click', () =>
  uploadImage('img-bg-file', '/api/upload/background', 'img-bg-status'));
document.getElementById('img-bg-delete').addEventListener('click', () =>
  deleteImage('/api/images/background', 'img-bg-status'));
document.getElementById('img-nd-upload').addEventListener('click', () =>
  uploadImage('img-nd-file', '/api/upload/needle', 'img-nd-status'));
document.getElementById('img-nd-delete').addEventListener('click', () =>
  deleteImage('/api/images/needle', 'img-nd-status'));

// ─── Warnings ─────────────────────────────────────────────────────────────────

let s_warnings = null;

async function loadWarnings() {
  try {
    const r = await fetch('/api/warnings');
    s_warnings = (await r.json()).warnings;
    renderWarnings(s_warnings);
  } catch (e) {
    toast('Failed to load warnings', 'err');
  }
}

function escHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                  .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function renderWarnings(warnings) {
  const container = document.getElementById('warnings-container');
  if (!container) return;
  container.innerHTML = warnings.map((w, i) => `
    <div class="card" style="margin-bottom:0.75rem">
      <div class="face-header">
        <h2>Slot ${i + 1}</h2>
        <label class="toggle">
          <input type="checkbox" id="warn-enabled-${i}" ${w.enabled ? 'checked' : ''}
                 onchange="onWarnEnabledChange(${i})">
          <span>Enabled</span>
        </label>
      </div>
      <div id="warn-body-${i}" ${w.enabled ? '' : 'style="opacity:0.45;pointer-events:none"'}>
        <div class="form-grid">
          <div class="field">
            <label>Field to monitor</label>
            <select id="warn-field-${i}">
              ${fieldOptHtml(w.field || 0)}
            </select>
          </div>
          <div class="field">
            <label>Label (shown on alert, leave blank for field name)</label>
            <input type="text" id="warn-label-${i}" maxlength="23" value="${escHtml(w.label || '')}">
          </div>
          <div class="field">
            <label>Lower threshold — warn if value &lt; this (0 = off)</label>
            <input type="number" step="any" id="warn-lo-${i}" value="${w.lower_threshold || 0}">
          </div>
          <div class="field">
            <label>Upper threshold — warn if value &gt; this (0 = off)</label>
            <input type="number" step="any" id="warn-hi-${i}" value="${w.upper_threshold || 0}">
          </div>
          <div class="field field-check">
            <label>
              <input type="checkbox" id="warn-hipri-${i}" ${w.high_priority ? 'checked' : ''}>
              High priority (replaces entire gauge display with full-screen alert)
            </label>
          </div>
        </div>
      </div>
    </div>`
  ).join('');
}

function onWarnEnabledChange(i) {
  const enabled = document.getElementById(`warn-enabled-${i}`).checked;
  const body    = document.getElementById(`warn-body-${i}`);
  body.style.opacity       = enabled ? '1' : '0.45';
  body.style.pointerEvents = enabled ? '' : 'none';
}

function collectWarnings() {
  return Array.from({ length: 8 }, (_, i) => ({
    field:           parseInt(document.getElementById(`warn-field-${i}`).value)    || 0,
    enabled:         document.getElementById(`warn-enabled-${i}`).checked,
    lower_threshold: parseFloat(document.getElementById(`warn-lo-${i}`).value)     || 0,
    upper_threshold: parseFloat(document.getElementById(`warn-hi-${i}`).value)     || 0,
    high_priority:   document.getElementById(`warn-hipri-${i}`).checked,
    label:           document.getElementById(`warn-label-${i}`).value              || '',
  }));
}

document.getElementById('save-warnings-btn').addEventListener('click', async () => {
  try {
    const r = await fetch('/api/warnings', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ warnings: collectWarnings() }),
    });
    const res = await r.json();
    toast(res.status === 'ok' ? 'Warnings saved' : 'Save failed',
          res.status === 'ok' ? 'ok' : 'err');
  } catch (e) {
    toast('Network error: ' + e.message, 'err');
  }
});
