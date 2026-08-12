/* Home: the grid of device cards.
 * Ports DeviceCard / HomePage from aircubetray/aircubeapp/ui/home.py.
 */

import { h, clear, pill, setPill, statusDot, toggle, progressBar, toast } from "../ui.js";
import { prefs } from "../prefs.js";
import { Sparkline, AirGauge } from "../charts.js";
import {
  readingQuality,
  airScore,
  qualityClass,
  qualityShortLabel,
  qualityStatusPill,
  cToF,
  updatedAgo,
  CO2_VALID_FLOOR,
} from "../quality.js";

/** Brightness restored when the LED is toggled back on. */
const LED_DEFAULT_PERCENT = 60;

class DeviceCard {
  constructor(device, onOpen) {
    this.device = device;
    this.onOpen = onOpen;

    this.dot = statusDot();
    this.name = h("span.device-name");
    this.typePill = pill("AirCube");
    this.statusPill = pill("Waiting...");

    this.vocValue = h("div", { class: "value", text: "--" });
    this.co2Value = h("div", { class: "value", text: "--" });
    this.co2Box = h(
      "div",
      this.co2Value,
      h("div", { class: "caption", text: "CO2 ppm" }),
    );

    this.spark = new Sparkline({ height: 48 });
    this.sparkCaption = h("div.spark-caption", { text: "VOC · last 2 hours" });

    this.secondary = [0, 1, 2, 3].map(() => ({
      value: h("div", { class: "value", text: "--" }),
      caption: h("div", { class: "caption" }),
    }));

    this.gauge = new AirGauge();
    this.syncBar = progressBar();
    this.syncBar.style.display = "none";
    this.updated = h("span.faint", { text: "Never updated" });

    this.ledToggle = toggle(false, (on) => {
      this.device
        .setBrightness(on ? LED_DEFAULT_PERCENT : 0)
        .catch((err) => toast(err.message, "err"));
    });

    this.el = h(
      "div.card.device-card",
      {
        role: "button",
        tabindex: "0",
        onclick: (event) => {
          if (event.target.closest(".toggle")) return;
          this.onOpen(this.device.id);
        },
        onkeydown: (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            this.onOpen(this.device.id);
          }
        },
      },
      h("div.row.header", this.dot, this.name, this.typePill, h("div.spacer"), this.statusPill),
      h(
        "div.device-body",
        h(
          "div.col",
          h(
            "div.primary-metrics",
            h("div", this.vocValue, h("div", { class: "caption", text: "VOC ppb" })),
            this.co2Box,
          ),
          h("div", this.spark.el, this.sparkCaption),
          h(
            "div.secondary-metrics",
            ...this.secondary.map((cell) => h("div", cell.value, cell.caption)),
          ),
        ),
        this.gauge.el,
      ),
      this.syncBar,
      h(
        "div.row",
        this.updated,
        h("div.spacer"),
        h("span.faint", { text: "LED" }),
        this.ledToggle,
      ),
    );

    this.refresh();
  }

  refresh() {
    const device = this.device;
    const reading = device.lastReading;
    const useF = prefs.get("useFahrenheit");

    this.name.textContent = device.name;
    this.typePill.textContent = device.modelLabel;

    if (device.isOnline && reading) {
      const quality = readingQuality(reading);
      const cls = qualityClass(quality);
      this.dot.className = `dot tinted ${cls}`;
      setPill(this.statusPill, qualityStatusPill(quality), cls);
      // SVGElement.className is read-only, so classes go through setAttribute.
      this.gauge.el.setAttribute("class", `gauge ${cls}`);
      this.gauge.setScore(airScore(reading), qualityShortLabel(quality));
    } else {
      this.dot.className = "dot";
      setPill(this.statusPill, device.isConnected ? "Waiting..." : "Offline", null);
      this.gauge.el.setAttribute("class", "gauge q-none");
      this.gauge.clear();
    }

    if (reading) {
      this.vocValue.textContent = String(reading.etvoc);
      this.co2Value.textContent = device.isPro ? String(reading.co2) : "--";

      const temp = useF ? cToF(reading.temperatureC) : reading.temperatureC;
      const cells = [
        [`${temp.toFixed(0)}°`, "TEMP"],
        [`${reading.humidity.toFixed(0)}%`, "HUMIDITY"],
        [reading.aqiUba ? String(reading.aqiUba) : "--", "AQI (UBA)"],
        device.isPro
          ? [`${reading.lux.toFixed(0)} lx`, "LIGHT"]
          : [String(reading.eco2), "eCO2 ppm"],
      ];
      cells.forEach(([value, caption], index) => {
        this.secondary[index].value.textContent = value;
        this.secondary[index].caption.textContent = caption;
      });
    }

    this.co2Box.style.display = device.isPro ? "" : "none";
    this.sparkCaption.textContent = `${device.isPro ? "CO2" : "VOC"} · last 2 hours`;
    this.updated.textContent = device.isConnected
      ? updatedAgo(device.lastUpdated)
      : "Disconnected";

    if (device.ledPercent != null) {
      this.ledToggle.setAttribute("aria-checked", String(device.ledPercent > 0));
    }

    this.refreshSparkline();
    this.refreshSync();
  }

  refreshSparkline() {
    const device = this.device;
    const slots = device.sparklineSlots(24);
    if (device.isPro) {
      this.spark.el.classList.remove("m-voc");
      this.spark.el.classList.add("m-co2");
      this.spark.el.style.color = "var(--m)";
      this.spark.setData(
        slots.filter((s) => s.co2Avg > CO2_VALID_FLOOR).map((s) => s.co2Avg),
      );
    } else {
      this.spark.el.classList.remove("m-co2");
      this.spark.el.classList.add("m-voc");
      this.spark.el.style.color = "var(--m)";
      this.spark.setData(slots.filter((s) => s.etvocAvg > 0).map((s) => s.etvocAvg));
    }
  }

  refreshSync() {
    const { isSyncing, syncProgress } = this.device;
    this.syncBar.style.display = isSyncing ? "" : "none";
    if (!isSyncing) return;
    if (syncProgress.total > 0) this.syncBar.setFraction(syncProgress.current / syncProgress.total);
    else this.syncBar.setIndeterminate();
  }
}

export class HomeView {
  constructor(registry, { onOpenDetail, onOpenSettings, onConnect }) {
    this.registry = registry;
    this.onOpenDetail = onOpenDetail;
    this.cards = new Map();

    this.grid = h("div.device-grid");
    this.empty = h(
      "div.empty-state",
      h("span.glyph", { text: "\u2b1c" }),
      h("div", { text: "No AirCubes yet." }),
      h("div", { text: "Plug one in over USB-C and click Connect." }),
    );

    this.el = h(
      "div.column",
      h(
        "div.page-header",
        h("h1.page-title", { text: "AirCube Home" }),
        h("div.spacer"),
        h("button.toolbtn", {
          type: "button",
          title: "Settings",
          "aria-label": "Settings",
          text: "\u2699",
          onclick: onOpenSettings,
        }),
        h("button.toolbtn", {
          type: "button",
          title: "Connect an AirCube",
          "aria-label": "Connect an AirCube",
          text: "+",
          onclick: onConnect,
        }),
      ),
      this.grid,
      this.empty,
    );
  }

  refresh() {
    const devices = this.registry.devices;
    const ids = new Set(devices.map((d) => d.id));

    for (const [id, card] of this.cards) {
      if (!ids.has(id)) {
        card.el.remove();
        this.cards.delete(id);
      }
    }

    for (const device of devices) {
      let card = this.cards.get(device.id);
      if (!card) {
        card = new DeviceCard(device, this.onOpenDetail);
        this.cards.set(device.id, card);
        this.grid.append(card.el);
      } else {
        card.refresh();
      }
    }

    this.empty.style.display = devices.length ? "none" : "";
  }

  destroy() {
    clear(this.grid);
    this.cards.clear();
  }
}
