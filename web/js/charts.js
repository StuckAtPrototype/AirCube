/* Chart primitives: sparkline, air gauge, history chart.
 *
 * Ports the hand-painted Qt widgets in aircubeapp/ui/widgets.py and
 * aircubeapp/ui/charts.py to SVG and uPlot, keeping the same geometry:
 * a 270-degree gauge with the gap at the bottom, sparklines with an 18%
 * area fill under a 1.6px line, and a history area fill fading 22% to 2%.
 */

import uPlot from "../vendor/uplot/uPlot.esm.js";

const SVG_NS = "http://www.w3.org/2000/svg";

const el = (name, attrs = {}) => {
  const node = document.createElementNS(SVG_NS, name);
  for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v);
  return node;
};

const cssVar = (node, name) =>
  getComputedStyle(node).getPropertyValue(name).trim();

// ------------------------------------------------------------------ sparkline

/**
 * Tiny area+line chart of the last ~2 hours of one metric.
 * Colour comes from the element's --m custom property, so callers just swap
 * the m-co2 / m-voc / m-temp / m-hum class.
 */
export class Sparkline {
  constructor({ height = 34 } = {}) {
    this.height = height;
    this.el = el("svg", {
      class: "sparkline",
      height,
      viewBox: `0 0 100 ${height}`,
      preserveAspectRatio: "none",
      "aria-hidden": "true",
    });
    this.area = el("path", { fill: "currentColor", "fill-opacity": "0.18", stroke: "none" });
    this.line = el("path", {
      fill: "none",
      stroke: "currentColor",
      "stroke-width": "1.6",
      "stroke-linejoin": "round",
      "stroke-linecap": "round",
      "vector-effect": "non-scaling-stroke",
    });
    this.el.append(this.area, this.line);
  }

  setData(values) {
    const points = values.filter((v) => Number.isFinite(v));
    if (points.length < 2) {
      this.area.setAttribute("d", "");
      this.line.setAttribute("d", "");
      return;
    }
    const pad = 2;
    const h = this.height;
    const min = Math.min(...points);
    const max = Math.max(...points);
    const span = max - min || 1;
    const coords = points.map((v, i) => {
      const x = pad + ((100 - 2 * pad) * i) / (points.length - 1);
      const y = pad + (h - 2 * pad) * (1 - (v - min) / span);
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    });
    const line = `M${coords.join("L")}`;
    this.line.setAttribute("d", line);
    const firstX = pad.toFixed(2);
    const lastX = (100 - pad).toFixed(2);
    const baseline = (h - pad).toFixed(2);
    this.area.setAttribute("d", `${line}L${lastX},${baseline}L${firstX},${baseline}Z`);
  }
}

// ---------------------------------------------------------------------- gauge

/**
 * 270-degree arc gauge with the gap at the bottom, 0-100 score in the middle.
 * The arc is drawn as a dashed circle rotated so its start sits bottom-left.
 */
export class AirGauge {
  constructor() {
    const size = 150;
    const radius = 57.5;
    this.radius = radius;
    this.circumference = 2 * Math.PI * radius;

    this.el = el("svg", {
      class: "gauge",
      viewBox: `0 0 ${size} ${size}`,
      role: "img",
    });

    const common = {
      cx: size / 2,
      cy: size / 2,
      r: radius,
      fill: "none",
      "stroke-width": 9,
      "stroke-linecap": "round",
      transform: `rotate(135 ${size / 2} ${size / 2})`,
    };
    this.track = el("circle", {
      ...common,
      stroke: "var(--card-border)",
      "stroke-opacity": "0.7",
      "stroke-dasharray": `${this.circumference * 0.75} ${this.circumference}`,
    });
    this.arc = el("circle", {
      ...common,
      stroke: "var(--q)",
      "stroke-dasharray": `0 ${this.circumference}`,
    });

    this.caption = el("text", {
      x: size / 2,
      y: size / 2 - 18,
      "text-anchor": "middle",
      fill: "var(--faint)",
      "font-size": "9",
      "font-weight": "600",
      "letter-spacing": "0.5",
    });
    this.caption.textContent = "AIR QUALITY";

    this.score = el("text", {
      x: size / 2,
      y: size / 2 + 12,
      "text-anchor": "middle",
      fill: "var(--text)",
      "font-size": "32",
      "font-weight": "600",
    });
    this.score.textContent = "--";

    this.verdict = el("text", {
      x: size / 2,
      y: size / 2 + 30,
      "text-anchor": "middle",
      fill: "var(--q)",
      "font-size": "12",
      "font-weight": "600",
    });

    const scaleAttrs = { y: size / 2 + 52, fill: "var(--faint)", "font-size": "9" };
    const low = el("text", { ...scaleAttrs, x: 26, "text-anchor": "start" });
    low.textContent = "0";
    const high = el("text", { ...scaleAttrs, x: size - 26, "text-anchor": "end" });
    high.textContent = "100";

    this.el.append(this.track, this.arc, this.caption, this.score, this.verdict, low, high);
  }

  setScore(score, shortLabel) {
    const clamped = Math.max(0, Math.min(100, score));
    // A sliver is always drawn so a perfect score still reads as "measured".
    const fraction = Math.max(clamped, 3) / 100;
    this.arc.setAttribute(
      "stroke-dasharray",
      `${this.circumference * 0.75 * fraction} ${this.circumference}`,
    );
    this.score.textContent = String(clamped);
    this.verdict.textContent = shortLabel;
    this.el.setAttribute("aria-label", `Air quality score ${clamped}, ${shortLabel}`);
  }

  clear() {
    this.arc.setAttribute("stroke-dasharray", `0 ${this.circumference}`);
    this.score.textContent = "--";
    this.verdict.textContent = "";
    this.el.setAttribute("aria-label", "No air quality reading yet");
  }
}

// -------------------------------------------------------------- history chart

export class HistoryChart {
  /**
   * @param {HTMLElement} host container the chart fills
   * @param {(point: {value: number, time: number} | null) => void} onScrub
   */
  constructor(host, onScrub) {
    this.host = host;
    this.onScrub = onScrub;
    this.plot = null;
    this.data = [[], []];
    this.format = (v) => String(v);

    this.observer = new ResizeObserver(() => this._resize());
    this.observer.observe(host);
  }

  _resize() {
    if (!this.plot) return;
    const { width, height } = this._size();
    if (width > 0 && height > 0) this.plot.setSize({ width, height });
  }

  _size() {
    return {
      width: Math.max(0, Math.floor(this.host.clientWidth)),
      height: Math.max(0, Math.floor(this.host.clientHeight)),
    };
  }

  /**
   * @param {Array<Array<[number, number]>>} segments contiguous runs of points
   * @param {string} colorVar CSS custom property holding the metric colour
   * @param {(v: number) => string} format value formatter for the scrub readout
   * @param {[number, number]} xRange visible time window in unix seconds
   */
  setData(segments, colorVar, format, xRange) {
    this.format = format;
    const xs = [];
    const ys = [];
    segments.forEach((segment, index) => {
      // A null between runs makes uPlot break the line across data gaps.
      if (index > 0 && segment.length) {
        xs.push(segment[0][0] - 1);
        ys.push(null);
      }
      for (const [x, y] of segment) {
        xs.push(x);
        ys.push(y);
      }
    });
    this.data = [xs, ys];
    this.color = cssVar(this.host, colorVar) || "#8b5cf6";
    this.xRange = xRange;
    this._render();
  }

  _render() {
    if (this.plot) {
      this.plot.destroy();
      this.plot = null;
    }
    const { width, height } = this._size();
    if (width === 0 || height === 0) return;

    const stroke = this.color;
    const grid = cssVar(this.host, "--card-border");
    const axisText = cssVar(this.host, "--faint");
    const font = `11px ${cssVar(document.documentElement, "--font") || "sans-serif"}`;

    const opts = {
      width,
      height,
      padding: [10, 8, 0, 0],
      legend: { show: false },
      cursor: {
        y: false,
        points: { size: 6, width: 2, stroke: () => stroke, fill: () => stroke },
        drag: { x: false, y: false },
      },
      scales: {
        x: { time: true, range: () => this.xRange },
        // Pad the value axis instead of forcing zero, so small swings stay readable.
        y: {
          range: (u, min, max) => {
            if (min == null || max == null) return [0, 1];
            const pad = (max - min || Math.abs(max) || 1) * 0.15;
            return [min - pad, max + pad];
          },
        },
      },
      axes: [
        {
          stroke: axisText,
          font,
          grid: { stroke: grid, width: 1 },
          ticks: { stroke: grid, width: 1, size: 4 },
        },
        {
          stroke: axisText,
          font,
          size: 46,
          grid: { stroke: grid, width: 1 },
          ticks: { show: false },
        },
      ],
      series: [
        {},
        {
          stroke,
          width: 2,
          spanGaps: false,
          points: { show: false },
          fill: (u) => {
            const ctx = u.ctx;
            const gradient = ctx.createLinearGradient(0, u.bbox.top, 0, u.bbox.top + u.bbox.height);
            gradient.addColorStop(0, withAlpha(stroke, 0.22));
            gradient.addColorStop(1, withAlpha(stroke, 0.02));
            return gradient;
          },
        },
      ],
      hooks: {
        setCursor: [
          (u) => {
            const idx = u.cursor.idx;
            if (idx == null || this.data[1][idx] == null) {
              this.onScrub?.(null);
            } else {
              this.onScrub?.({ value: this.data[1][idx], time: this.data[0][idx] });
            }
          },
        ],
      },
    };

    this.plot = new uPlot(opts, this.data, this.host);
  }

  destroy() {
    this.observer.disconnect();
    this.plot?.destroy();
    this.plot = null;
  }
}

/** Accepts #rgb, #rrggbb and rgb()/rgba() strings, since these come from CSS. */
function withAlpha(color, alpha) {
  if (color.startsWith("#")) {
    let hex = color.slice(1);
    if (hex.length === 3) hex = [...hex].map((c) => c + c).join("");
    const n = parseInt(hex, 16);
    return `rgba(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}, ${alpha})`;
  }
  const nums = color.match(/[\d.]+/g);
  if (nums && nums.length >= 3) {
    return `rgba(${nums[0]}, ${nums[1]}, ${nums[2]}, ${alpha})`;
  }
  return color;
}
