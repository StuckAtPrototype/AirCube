/**
 * AirCube Dashboard Card
 * A self-updating Lovelace custom card for StuckAtPrototype AirCube sensors.
 *
 * AUTO-DISCOVERY: Queries the HA device registry for all devices where
 *   manufacturer === "StuckAtPrototype"  AND  model === "AirCube"
 * New AirCubes appear automatically — no config changes needed.
 *
 * LIVE UPDATES: History is fetched once on init; after that, current values
 * update via `state_changed` event subscription (no repeated recorder hits).
 * New points are appended to the in-memory series as they arrive.
 *
 * INSTALL:
 *   1. Copy this file to:  /config/www/aircube-dashboard-card.js
 *   2. In HA → Settings → Dashboards → Resources → Add Resource:
 *        URL:  /local/aircube-dashboard-card.js
 *        Type: JavaScript Module
 *      (If Resources is missing, enable Advanced Mode in your user profile.)
 *   3. Add a card to any Lovelace view:
 *        type: custom:aircube-dashboard-card
 *   4. Hard-refresh the browser (Ctrl-Shift-R) after updating the file.
 *
 * OPTIONAL CONFIG (all have defaults):
 *   type: custom:aircube-dashboard-card
 *   graph_span_minutes: 120   # history window (default: 120)
 *   rerender_ms: 1000         # throttle graph redraws (default: 1000)
 */

const PALETTE = [
  "#38BDF8", // sky blue
  "#FB923C", // amber
  "#4ADE80", // green
  "#F472B6", // pink
  "#A78BFA", // violet
  "#FACC15", // yellow
  "#34D399", // emerald
  "#F87171", // red
  "#60A5FA", // blue
  "#C084FC", // purple
];

// ENS161 AQI-S bands (what the AirCube firmware actually reports on cluster 0xFC01)
// ENS161 datasheet: 0-50 excellent, 51-100 good, 101-150 moderate,
// 151-200 poor, 201-300 unhealthy, 301+ very unhealthy
const AQI_BANDS = [
  { max:  50, label: "Excellent", color: "#4ADE80" },
  { max: 100, label: "Good",      color: "#A3E635" },
  { max: 150, label: "Moderate",  color: "#FACC15" },
  { max: 200, label: "Poor",      color: "#FB923C" },
  { max: 300, label: "Unhealthy", color: "#F87171" },
  { max: Infinity, label: "Hazardous", color: "#B91C1C" },
];

function aqiLabel(v) {
  if (v === null || v === undefined || isNaN(v)) return "—";
  // Defensive: if firmware ever sends UBA 1-5 scale, still render sensibly
  if (v <= 5) {
    return ["", "Excellent", "Good", "Moderate", "Poor", "Unhealthy"][Math.round(v)] || String(v);
  }
  for (const b of AQI_BANDS) if (v <= b.max) return b.label;
  return "Hazardous";
}

function aqiColor(v) {
  if (v === null || v === undefined || isNaN(v)) return "#4a5568";
  if (v <= 5) {
    // UBA fallback
    return ["#4a5568","#4ADE80","#A3E635","#FACC15","#FB923C","#F87171"][Math.round(v)] || "#4a5568";
  }
  for (const b of AQI_BANDS) if (v <= b.max) return b.color;
  return "#B91C1C";
}

const SENSORS = [
  {
    key: "temperature",
    label: "Temperature",
    unit: "°F",
    icon: "🌡",
    yMin: null, yMax: null,
    bands: [],
    format: v => `${v.toFixed(1)}°F`,
  },
  {
    key: "humidity",
    label: "Humidity",
    unit: "%",
    icon: "💧",
    yMin: 0, yMax: 100,
    bands: [],
    format: v => `${v.toFixed(1)}%`,
  },
  {
    key: "carbon_dioxide",
    label: "CO₂ eq (eCO2)",
    unit: "ppm",
    icon: "🫁",
    yMin: 400, yMax: null,
    bands: [
      { y: 1000, color: "#FACC15", label: "1000 — Ventilate" },
      { y: 2000, color: "#F87171", label: "2000 — Poor" },
    ],
    format: v => `${Math.round(v)} ppm`,
  },
  {
    key: "volatile_organic_compounds_parts",
    label: "TVOC",
    unit: "ppb",
    icon: "🧪",
    yMin: 0, yMax: null,
    bands: [
      { y: 220, color: "#FACC15", label: "220 — Elevated" },
      { y: 660, color: "#F87171", label: "660 — High" },
    ],
    format: v => `${Math.round(v)} ppb`,
  },
  {
    key: "air_quality_index",
    label: "Air Quality Index (ENS161 AQI-S)",
    unit: "",
    icon: "🟢",
    yMin: 0, yMax: null,
    bands: [
      { y:  50, color: "#A3E635", label: " 50 — Good" },
      { y: 100, color: "#FACC15", label: "100 — Moderate" },
      { y: 150, color: "#FB923C", label: "150 — Poor" },
      { y: 200, color: "#F87171", label: "200 — Unhealthy" },
    ],
    format: v => `${Math.round(v)} (${aqiLabel(v)})`,
  },
];

// ─── Sparkline SVG renderer ───────────────────────────────────────────────────
function renderSparkline(seriesData, sensor, width, height) {
  const PAD = { top: 10, right: 14, bottom: 32, left: 56 };
  const W = width - PAD.left - PAD.right;
  const H = height - PAD.top - PAD.bottom;

  const allVals = seriesData.flatMap(s => s.points.map(p => p.v));
  if (!allVals.length) {
    return `<svg width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" xmlns="http://www.w3.org/2000/svg">
      <rect width="${width}" height="${height}" fill="#0f1117" rx="10"/>
      <text x="50%" y="50%" text-anchor="middle" fill="#4a5568" font-size="11" font-family="'DM Mono', monospace">WAITING FOR DATA…</text>
    </svg>`;
  }

  let yMin = sensor.yMin ?? Math.min(...allVals);
  let yMax = sensor.yMax ?? Math.max(...allVals);
  // Expand range slightly so lines don't touch edges
  const vMin = Math.min(...allVals);
  const vMax = Math.max(...allVals);
  if (sensor.yMin === null) yMin = Math.min(yMin, vMin);
  if (sensor.yMax === null) yMax = Math.max(yMax, vMax);
  if (yMin === yMax) { yMin -= 1; yMax += 1; }
  const yPad = (yMax - yMin) * 0.08;
  yMin -= yPad; yMax += yPad;

  const allTimes = seriesData.flatMap(s => s.points.map(p => p.t));
  const tMin = Math.min(...allTimes);
  const tMax = Math.max(...allTimes);
  const tRange = tMax - tMin || 1;

  const px = t => PAD.left + ((t - tMin) / tRange) * W;
  const py = v => PAD.top + H - ((v - yMin) / (yMax - yMin)) * H;

  let svg = `<svg width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" xmlns="http://www.w3.org/2000/svg" font-family="'DM Mono', 'Courier New', monospace">`;
  svg += `<rect width="${width}" height="${height}" fill="#0f1117" rx="10"/>`;

  // Horizontal grid + y labels
  const ticks = 4;
  for (let i = 0; i <= ticks; i++) {
    const v = yMin + (i / ticks) * (yMax - yMin);
    const y = py(v);
    let label;
    if (sensor.key === "air_quality_index") {
      label = Math.round(v);
    } else if (Number.isInteger(v)) {
      label = v;
    } else {
      label = v.toFixed(1);
    }
    svg += `<line x1="${PAD.left}" y1="${y}" x2="${PAD.left + W}" y2="${y}" stroke="#1e2433" stroke-width="1"/>`;
    svg += `<text x="${PAD.left - 6}" y="${y + 4}" text-anchor="end" fill="#4a5568" font-size="9">${label}</text>`;
  }

  // Reference bands
  for (const band of sensor.bands) {
    if (band.y >= yMin && band.y <= yMax) {
      const y = py(band.y);
      svg += `<line x1="${PAD.left}" y1="${y}" x2="${PAD.left + W}" y2="${y}" stroke="${band.color}" stroke-width="1" stroke-dasharray="4 3" opacity="0.55"/>`;
      svg += `<text x="${PAD.left + W - 4}" y="${y - 3}" text-anchor="end" fill="${band.color}" font-size="8" opacity="0.75">${band.label}</text>`;
    }
  }

  // Series
  for (const series of seriesData) {
    if (!series.points.length) continue;
    const pts = series.points.map(p => `${px(p.t).toFixed(1)},${py(p.v).toFixed(1)}`).join(" ");
    const firstX = px(series.points[0].t).toFixed(1);
    const lastX  = px(series.points[series.points.length - 1].t).toFixed(1);
    const bottom = (PAD.top + H).toFixed(1);

    svg += `<polygon points="${firstX},${bottom} ${pts} ${lastX},${bottom}" fill="${series.color}" opacity="0.08"/>`;
    svg += `<polyline points="${pts}" fill="none" stroke="${series.color}" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"/>`;
    const last = series.points[series.points.length - 1];
    svg += `<circle cx="${px(last.t).toFixed(1)}" cy="${py(last.v).toFixed(1)}" r="3" fill="${series.color}"/>`;
  }

  // X-axis time labels
  const labelCount = 5;
  for (let i = 0; i <= labelCount; i++) {
    const t = tMin + (i / labelCount) * tRange;
    const x = px(t);
    const d = new Date(t);
    const label = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    svg += `<text x="${x}" y="${PAD.top + H + 16}" text-anchor="middle" fill="#4a5568" font-size="9">${label}</text>`;
  }

  svg += `</svg>`;
  return svg;
}

// ─── Card ─────────────────────────────────────────────────────────────────────
class AirCubeDashboardCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._devices       = [];
    this._history       = {};   // deviceId → { sensorKey → [{t,v}] }
    this._current       = {};   // deviceId → { sensorKey → {v, t} }
    this._entityIndex   = {};   // entity_id → { deviceId, sensorKey }
    this._config        = {};
    this._hass          = null;
    this._initialized   = false;
    this._unsubState    = null;
    this._rerenderTimer = null;
    this._dirty         = false;
  }

  setConfig(config) {
    this._config = {
      graph_span_minutes: config.graph_span_minutes ?? 120,
      rerender_ms:        config.rerender_ms        ?? 1000,
    };
  }

  set hass(hass) {
    this._hass = hass;
    if (!this._initialized) {
      this._initialized = true;
      this._init();
    }
  }

  async _init() {
    this._render();
    await this._discover();
    await this._loadHistory();
    await this._subscribeState();
    this._scheduleRender();
  }

  disconnectedCallback() {
    if (this._unsubState) { this._unsubState(); this._unsubState = null; }
    if (this._rerenderTimer) { clearInterval(this._rerenderTimer); this._rerenderTimer = null; }
  }

  // ── Discover AirCubes ─────────────────────────────────────────────────────
  async _discover() {
    try {
      const devices = await this._hass.callWS({ type: "config/device_registry/list" });
      this._devices = devices.filter(d =>
        d.manufacturer === "StuckAtPrototype" && d.model === "AirCube"
      );
    } catch (e) {
      console.error("[AirCube] Device discovery failed:", e);
      this._devices = [];
    }
  }

  async _getEntitiesForDevice(deviceId) {
    try {
      const all = await this._hass.callWS({ type: "config/entity_registry/list" });
      return all.filter(e => e.device_id === deviceId);
    } catch { return []; }
  }

  _findEntity(entities, sensorKey) {
    return entities.find(e => {
      const eid = e.entity_id || "";
      const name = (e.original_name ?? "").toLowerCase().replace(/ /g, "_");
      if (sensorKey === "carbon_dioxide") {
        return eid.includes("carbon_dioxide") || name === "carbon_dioxide" || eid.includes("eco2");
      }
      if (sensorKey === "volatile_organic_compounds_parts") {
        return eid.includes("volatile_organic_compounds") || eid.includes("tvoc") || eid.includes("etvoc");
      }
      if (sensorKey === "air_quality_index") {
        return eid.includes("air_quality_index") || eid.includes("aqi");
      }
      return eid.includes(sensorKey);
    });
  }

  // ── Load history once on init ─────────────────────────────────────────────
  async _loadHistory() {
    const spanMs = this._config.graph_span_minutes * 60 * 1000;
    const start  = new Date(Date.now() - spanMs).toISOString();
    this._entityIndex = {};

    for (const device of this._devices) {
      this._history[device.id] = {};
      this._current[device.id] = {};
      const entities = await this._getEntitiesForDevice(device.id);

      for (const sensor of SENSORS) {
        const entity = this._findEntity(entities, sensor.key);
        if (!entity) {
          this._history[device.id][sensor.key] = [];
          continue;
        }
        this._entityIndex[entity.entity_id] = { deviceId: device.id, sensorKey: sensor.key };

        try {
          const hist = await this._hass.callApi(
            "GET",
            `history/period/${start}?filter_entity_id=${entity.entity_id}&minimal_response=true`
          );
          const series = (hist?.[0] ?? [])
            .filter(s => s.state !== "unavailable" && s.state !== "unknown")
            .map(s => ({ t: new Date(s.last_changed).getTime(), v: parseFloat(s.state) }))
            .filter(p => !isNaN(p.v));
          this._history[device.id][sensor.key] = series;
          if (series.length) {
            const last = series[series.length - 1];
            this._current[device.id][sensor.key] = { v: last.v, t: last.t };
          }
        } catch (e) {
          this._history[device.id][sensor.key] = [];
        }

        // Seed current from hass.states if present (fresher than history)
        const st = this._hass.states[entity.entity_id];
        if (st && st.state !== "unavailable" && st.state !== "unknown") {
          const v = parseFloat(st.state);
          if (!isNaN(v)) {
            this._current[device.id][sensor.key] = {
              v, t: new Date(st.last_changed).getTime(),
            };
          }
        }
      }
    }
    this._dirty = true;
  }

  // ── Subscribe to live state_changed events ───────────────────────────────
  async _subscribeState() {
    if (!this._hass?.connection) return;
    try {
      this._unsubState = await this._hass.connection.subscribeEvents(
        (ev) => this._onStateChanged(ev),
        "state_changed"
      );
    } catch (e) {
      console.error("[AirCube] subscribeEvents failed:", e);
    }
  }

  _onStateChanged(ev) {
    const { entity_id, new_state } = ev.data;
    const idx = this._entityIndex[entity_id];
    if (!idx) return;
    if (!new_state || new_state.state === "unavailable" || new_state.state === "unknown") return;
    const v = parseFloat(new_state.state);
    if (isNaN(v)) return;
    const t = new Date(new_state.last_changed).getTime();

    const series = this._history[idx.deviceId]?.[idx.sensorKey];
    if (series) {
      const last = series[series.length - 1];
      if (!last || t > last.t) series.push({ t, v });
      // Trim anything older than graph_span_minutes
      const cutoff = Date.now() - this._config.graph_span_minutes * 60 * 1000;
      while (series.length && series[0].t < cutoff) series.shift();
    }
    this._current[idx.deviceId][idx.sensorKey] = { v, t };
    this._dirty = true;
  }

  // ── Throttled rerender ───────────────────────────────────────────────────
  _scheduleRender() {
    if (this._rerenderTimer) return;
    this._rerenderTimer = setInterval(() => {
      if (!this._dirty) return;
      this._dirty = false;
      this._render();
    }, this._config.rerender_ms);
  }

  // ── Render ───────────────────────────────────────────────────────────────
  _render() {
    const root = this.shadowRoot;
    const devices = this._devices;
    const cardW  = this.offsetWidth || 600;
    const graphW = Math.max(cardW - 36, 300);
    const graphH = 170;

    const legendHTML = devices.map((d, i) => {
      const color = PALETTE[i % PALETTE.length];
      const name  = d.name_by_user ?? d.name ?? `AirCube ${i + 1}`;
      const aqiVal = this._current[d.id]?.air_quality_index?.v;
      const aqiDot = aqiVal != null
        ? `<span class="aqi-dot" style="background:${aqiColor(aqiVal)}" title="${aqiLabel(aqiVal)}"></span>`
        : "";
      return `<span class="legend-item">
        <span class="legend-dot" style="background:${color}"></span>
        <span class="legend-label">${name}</span>
        ${aqiDot}
      </span>`;
    }).join("");

    const currentRow = (sensor) => devices.map((d, i) => {
      const color = PALETTE[i % PALETTE.length];
      const cur   = this._current[d.id]?.[sensor.key];
      const label = cur ? sensor.format(cur.v) : "—";
      const extraStyle = (sensor.key === "air_quality_index" && cur)
        ? `;background:${aqiColor(cur.v)}20;padding:2px 6px;border-radius:4px`
        : "";
      return `<span class="cur-val" style="color:${color}${extraStyle}">${label}</span>`;
    }).join("");

    const graphsHTML = SENSORS.map(sensor => {
      const seriesData = devices.map((d, i) => ({
        color:  PALETTE[i % PALETTE.length],
        name:   d.name_by_user ?? d.name ?? `AirCube ${i + 1}`,
        points: this._history[d.id]?.[sensor.key] ?? [],
      }));
      const svg = renderSparkline(seriesData, sensor, graphW, graphH);
      return `
        <div class="graph-block">
          <div class="graph-header">
            <span class="graph-icon">${sensor.icon}</span>
            <span class="graph-title">${sensor.label}</span>
            <span class="cur-row">${currentRow(sensor)}</span>
          </div>
          <div class="graph-wrap">${svg}</div>
        </div>`;
    }).join("");

    const emptyState = devices.length === 0
      ? `<div class="loading">
           <div class="spinner"></div>
           <p>Scanning for AirCube devices…</p>
           <p class="sub">Looking for manufacturer: StuckAtPrototype / model: AirCube</p>
         </div>`
      : "";

    root.innerHTML = `
      <style>
        :host { display: block; font-family: 'DM Mono', 'Courier New', monospace; }
        .card {
          background: #0b0e15;
          border: 1px solid #1e2433;
          border-radius: 14px;
          overflow: hidden;
          color: #e2e8f0;
        }
        .card-header {
          display: flex;
          align-items: center;
          justify-content: space-between;
          padding: 16px 20px 10px;
          border-bottom: 1px solid #1e2433;
        }
        .card-title {
          font-size: 15px;
          font-weight: 700;
          letter-spacing: 0.05em;
          color: #f8fafc;
        }
        .device-count {
          font-size: 11px;
          color: #64748b;
          letter-spacing: 0.08em;
        }
        .legend {
          display: flex;
          flex-wrap: wrap;
          gap: 14px;
          padding: 10px 20px;
          border-bottom: 1px solid #1e2433;
        }
        .legend-item {
          display: flex;
          align-items: center;
          gap: 6px;
          font-size: 11px;
          color: #94a3b8;
        }
        .legend-dot {
          width: 10px; height: 10px;
          border-radius: 2px;
          flex-shrink: 0;
        }
        .aqi-dot {
          width: 8px; height: 8px;
          border-radius: 50%;
          box-shadow: 0 0 6px currentColor;
          flex-shrink: 0;
        }
        .legend-label { letter-spacing: 0.04em; }
        .graphs { padding: 12px 16px; display: flex; flex-direction: column; gap: 14px; }
        .graph-block {
          background: #0f1117;
          border: 1px solid #1e2433;
          border-radius: 10px;
          overflow: hidden;
        }
        .graph-header {
          display: flex;
          align-items: center;
          gap: 10px;
          padding: 8px 14px;
          border-bottom: 1px solid #1e2433;
        }
        .graph-icon { font-size: 14px; }
        .graph-title {
          font-size: 12px;
          font-weight: 600;
          color: #cbd5e1;
          letter-spacing: 0.06em;
          flex: 1;
        }
        .cur-row { display: flex; gap: 10px; align-items: center; }
        .cur-val {
          font-size: 12px;
          font-weight: 700;
          letter-spacing: 0.04em;
        }
        .graph-wrap { line-height: 0; }
        .graph-wrap svg { width: 100%; height: auto; display: block; }
        .loading {
          padding: 40px;
          text-align: center;
          color: #4a5568;
        }
        .loading p { margin: 6px 0; font-size: 13px; }
        .loading .sub { font-size: 10px; color: #2d3748; }
        .spinner {
          width: 28px; height: 28px;
          border: 2px solid #1e2433;
          border-top-color: #38BDF8;
          border-radius: 50%;
          animation: spin 0.8s linear infinite;
          margin: 0 auto 12px;
        }
        @keyframes spin { to { transform: rotate(360deg); } }
        .footer {
          padding: 8px 20px;
          font-size: 9px;
          color: #2d3748;
          border-top: 1px solid #1a1f2e;
          letter-spacing: 0.06em;
        }
      </style>
      <ha-card>
        <div class="card">
          <div class="card-header">
            <span class="card-title">🌬 AIRCUBE AIR QUALITY</span>
            <span class="device-count">${devices.length} DEVICE${devices.length !== 1 ? "S" : ""} · LIVE</span>
          </div>
          ${devices.length ? `<div class="legend">${legendHTML}</div>` : ""}
          <div class="graphs">
            ${emptyState}
            ${devices.length ? graphsHTML : ""}
          </div>
          <div class="footer">
            StuckAtPrototype AirCube · ENS161 VOC + ENS210 Temp/Humidity · ${this._config.graph_span_minutes}min history
          </div>
        </div>
      </ha-card>`;
  }

  getCardSize() { return 12; }

  static getConfigElement() {
    return document.createElement("div");
  }

  static getStubConfig() {
    return { graph_span_minutes: 120, rerender_ms: 1000 };
  }
}

customElements.define("aircube-dashboard-card", AirCubeDashboardCard);

window.customCards = window.customCards || [];
window.customCards.push({
  type:        "aircube-dashboard-card",
  name:        "AirCube Dashboard",
  description: "Auto-discovers StuckAtPrototype AirCube sensors and renders live trend graphs.",
});
