/* Air-quality verdicts, thresholds and formatting.
 *
 * Port of aircubetray/aircubeapp/models.py, which itself mirrors
 * AirCube_iOS/AirCube/Theme.swift. Keep the three in sync: a cube should read
 * the same on the phone, the tray and the web.
 */

export const GOOD = 0;
export const FAIR = 1;
export const POOR = 2;
export const BAD = 3;

const QUALITY_CLASS = ["q-good", "q-fair", "q-poor", "q-bad"];
const LABEL = ["Air is good", "Air is OK", "Air is poor", "Air is bad"];
const SHORT_LABEL = ["Good", "OK", "Poor", "Bad"];
const STATUS_PILL = ["Good", "Elevated", "Ventilate", "Ventilate"];

const ADVICE = [
  "Everything looks great. Enjoy the fresh air.",
  "Air quality is slightly elevated. A bit of fresh air wouldn't hurt.",
  "Air quality is poor. Open a window or increase ventilation.",
  "Air quality is bad. Ventilate this room now.",
];

export const qualityClass = (q) => QUALITY_CLASS[q] ?? "q-none";
export const qualityLabel = (q) => LABEL[q];
export const qualityShortLabel = (q) => SHORT_LABEL[q];
export const qualityStatusPill = (q) => STATUS_PILL[q];
export const qualityAdvice = (q) => ADVICE[q];

export function vocQuality(ppb) {
  if (ppb <= 220) return GOOD;
  if (ppb <= 660) return FAIR;
  if (ppb <= 2200) return POOR;
  return BAD;
}

export function co2Quality(ppm) {
  if (ppm < 800) return GOOD;
  if (ppm < 1200) return FAIR;
  if (ppm < 2000) return POOR;
  return BAD;
}

const VOC_SCORE_THRESHOLDS = [0, 220, 660, 2200, 5500];
const CO2_SCORE_THRESHOLDS = [400, 800, 1200, 2000, 3000];

/** Map a value onto 0-100 given the 5 band-threshold edges. */
function bandScore(value, thresholds) {
  if (value <= thresholds[0]) return 0;
  for (let i = 1; i < thresholds.length; i++) {
    if (value <= thresholds[i]) {
      const lo = thresholds[i - 1];
      const hi = thresholds[i];
      return (i - 1) * 25 + (25 * (value - lo)) / (hi - lo);
    }
  }
  return 100;
}

/** Overall verdict: worst of VOC and, on Pro, true CO2. */
export function readingQuality(r) {
  let q = vocQuality(r.etvoc);
  if (r.isPro) q = Math.max(q, co2Quality(r.co2));
  return q;
}

/** 0-100 air score, lower is better. */
export function airScore(r) {
  let score = bandScore(r.etvoc, VOC_SCORE_THRESHOLDS);
  if (r.isPro) score = Math.max(score, bandScore(r.co2, CO2_SCORE_THRESHOLDS));
  return Math.round(score);
}

// --------------------------------------------------------------- tile status

export function co2TileStatus(ppm) {
  if (ppm < 800) return "Fresh";
  if (ppm < 1200) return "Elevated";
  if (ppm < 2000) return "High";
  return "Very high";
}

export function vocTileStatus(ppb) {
  if (ppb <= 220) return "Low";
  if (ppb <= 660) return "Moderate";
  if (ppb <= 2200) return "Elevated";
  return "High";
}

export function humTileStatus(pct) {
  if (pct < 30) return "Dry";
  if (pct <= 60) return "Ideal";
  return "Humid";
}

export function tempTileStatus(c) {
  if (c < 18) return "Cool";
  if (c < 26) return "Ideal";
  if (c < 30) return "Warm";
  return "Hot";
}

export function tileStatusQuality(key, value) {
  if (key === "co2") return co2Quality(value);
  if (key === "voc") return vocQuality(value);
  if (key === "hum") return value < 30 || value > 60 ? FAIR : GOOD;
  if (key === "temp") {
    if (value >= 18 && value < 26) return GOOD;
    if (value < 30) return FAIR;
    return POOR;
  }
  return GOOD;
}

export function tileStatus(key, value) {
  const text =
    key === "co2"
      ? co2TileStatus(value)
      : key === "voc"
        ? vocTileStatus(value)
        : key === "hum"
          ? humTileStatus(value)
          : tempTileStatus(value);
  return { text, quality: tileStatusQuality(key, value) };
}

// ------------------------------------------------------------------ metrics

/** CO2 readings at or below this are treated as missing (iOS HistoryProcessing). */
export const CO2_VALID_FLOOR = 300;
const SEGMENT_MAX_SEQ_GAP = 3;
const SEGMENT_MAX_TIME_GAP_S = 1200;

export const HISTORY_METRICS = [
  { key: "co2", label: "CO2", unit: "ppm", colorClass: "m-co2", decimals: 0 },
  { key: "voc", label: "VOC", unit: "ppb", colorClass: "m-voc", decimals: 0 },
  { key: "temp", label: "Temperature", unit: "°", colorClass: "m-temp", decimals: 1 },
  { key: "hum", label: "Humidity", unit: "%", colorClass: "m-hum", decimals: 1 },
];

export const HISTORY_RANGES = [
  { key: "24h", label: "Past 24h", seconds: 86400 },
  { key: "3d", label: "Past 3d", seconds: 259200 },
  { key: "7d", label: "Past 7d", seconds: 604800 },
];

/** avg/min/max for one metric out of a history slot. */
export function slotValues(metricKey, s) {
  switch (metricKey) {
    case "co2":
      return [s.co2Avg, s.co2Min, s.co2Max];
    case "voc":
      return [s.etvocAvg, s.etvocMin, s.etvocMax];
    case "temp":
      return [s.tempAvg, s.tempMin, s.tempMax];
    default:
      return [s.humAvg, s.humMin, s.humMax];
  }
}

export const metricFloor = (key) => (key === "co2" ? CO2_VALID_FLOOR : 0);

/** Wrapping u16 sequence distance (sequences wrap at 0xFFFE). */
export const seqDistance = (newer, older) => (newer - older) & 0xffff;

/** Split slots into contiguous runs so charts break across gaps. */
export function historySegments(slots) {
  const sorted = [...slots].sort((a, b) => a.sequence - b.sequence);
  const segments = [];
  let current = [];
  for (const slot of sorted) {
    if (current.length) {
      const prev = current[current.length - 1];
      const seqGap = seqDistance(slot.sequence, prev.sequence);
      const timeGap = Math.abs(slot.timestamp - prev.timestamp);
      if (seqGap > SEGMENT_MAX_SEQ_GAP || timeGap > SEGMENT_MAX_TIME_GAP_S) {
        segments.push(current);
        current = [];
      }
    }
    current.push(slot);
  }
  if (current.length) segments.push(current);
  return segments;
}

// --------------------------------------------------------------- formatting

export const cToF = (c) => (c * 9) / 5 + 32;

export function formatTemp(c, fahrenheit, decimals = 1) {
  const v = fahrenheit ? cToF(c) : c;
  return `${v.toFixed(decimals)}°${fahrenheit ? "F" : "C"}`;
}

/** "just now" / "5 min ago" / "2 hr ago" / "3 d ago", as the tray words it. */
export function updatedAgo(timestampS) {
  if (!timestampS) return "Never updated";
  const delta = Date.now() / 1000 - timestampS;
  if (delta < 90) return "Updated just now";
  if (delta < 3600) return `Updated ${Math.round(delta / 60)} min ago`;
  if (delta < 86400) return `Updated ${Math.round(delta / 3600)} hr ago`;
  return `Updated ${Math.round(delta / 86400)} d ago`;
}

export function formatClock(timestampS) {
  return new Date(timestampS * 1000).toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
  });
}

export function formatDayTime(timestampS) {
  return new Date(timestampS * 1000).toLocaleString([], {
    weekday: "short",
    hour: "2-digit",
    minute: "2-digit",
  });
}
