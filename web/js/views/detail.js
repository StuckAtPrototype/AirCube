/* Device detail: hero verdict and gauge, metric tiles, LED brightness,
 * advanced drawer, and the history chart.
 * Ports DetailPage from aircubetray/aircubeapp/ui/detail.py.
 */

import {
  h,
  pill,
  setPill,
  statusDot,
  pillPicker,
  slider,
  progressBar,
  openMenu,
  toast,
  confirmDialog,
  promptDialog,
  downloadFile,
} from "../ui.js";
import { prefs } from "../prefs.js";
import { Sparkline, AirGauge, HistoryChart } from "../charts.js";
import {
  readingQuality,
  airScore,
  qualityClass,
  qualityLabel,
  qualityShortLabel,
  qualityAdvice,
  qualityStatusPill,
  tileStatus,
  slotValues,
  liveMetricValue,
  metricFloor,
  historySegments,
  HISTORY_METRICS,
  HISTORY_RANGES,
  CO2_VALID_FLOOR,
  cToF,
  updatedAgo,
  formatClock,
  formatDayTime,
  GOOD,
} from "../quality.js";

class MetricTile {
  constructor(key, caption, colorClass) {
    this.key = key;
    this.caption = h("span.caption", { text: caption });
    this.pill = pill("--");
    this.value = h("div.value", { text: "--" });
    this.spark = new Sparkline({ height: 26 });
    this.spark.el.classList.add(colorClass);
    this.spark.el.style.color = "var(--m)";

    this.el = h(
      "div.card.metric-tile",
      h("div.row", this.caption, h("div.spacer"), this.pill),
      this.value,
      this.spark.el,
    );
  }

  set(valueText, status) {
    this.value.textContent = valueText;
    setPill(this.pill, status.text, qualityClass(status.quality));
  }
}

export class DetailView {
  constructor(registry, { onBack, onFlash }) {
    this.registry = registry;
    this.onBack = onBack;
    this.onFlash = onFlash;
    this.device = null;
    this.metric = HISTORY_METRICS[0];
    this.rangeSeconds = HISTORY_RANGES[0].seconds;

    this._build();
  }

  _build() {
    // --- header ---------------------------------------------------------
    this.title = h("h1.detail-title", { text: "AirCube" });
    this.typePill = pill("AirCube");
    this.connDot = statusDot();
    this.connLabel = h("span.faint");
    this.backBtn = h("button.toolbtn", {
      type: "button",
      text: "\u2190",
      title: "Back to all AirCubes",
      "aria-label": "Back to all AirCubes",
      onclick: () => this.onBack(),
    });
    this.menuBtn = h("button.toolbtn", {
      type: "button",
      text: "\u22ef",
      title: "More actions",
      "aria-label": "More actions",
      onclick: () => this._openMenu(),
    });
    this.syncBar = progressBar();
    this.syncBar.style.visibility = "hidden";

    // --- hero -----------------------------------------------------------
    this.heroTitle = h("div.hero-title", { text: "Waiting for data..." });
    this.heroAdvice = h("div.hero-advice");
    this.heroPill = pill("No action needed");
    this.gauge = new AirGauge();
    this.hero = h(
      "div.card.hero.q-none",
      h("div.hero-text", this.heroTitle, this.heroAdvice, this.heroPill),
      this.gauge.el,
    );

    // --- tiles ----------------------------------------------------------
    this.tiles = {
      co2: new MetricTile("co2", "CO2", "m-co2"),
      voc: new MetricTile("voc", "VOC", "m-voc"),
      hum: new MetricTile("hum", "HUMIDITY", "m-hum"),
      temp: new MetricTile("temp", "TEMPERATURE", "m-temp"),
    };

    // --- brightness -----------------------------------------------------
    this.briValue = h("span.muted", { text: "--%" });
    this.briSlider = slider({
      onInput: (v) => {
        this.briValue.textContent = `${v}%`;
      },
      onCommit: (v) => {
        this.device?.setBrightness(v).catch((err) => toast(err.message, "err"));
      },
    });
    this.briCard = h(
      "div.card",
      h(
        "div.row",
        h("h2.card-title", { text: "LED brightness" }),
        h("div.spacer"),
        this.briValue,
      ),
      h("div", { style: { marginTop: "8px" } }, this.briSlider),
    );

    // --- advanced -------------------------------------------------------
    this.advRows = {};
    const advGrid = h("dl.advanced-grid");
    for (const [key, caption] of [
      ["eco2", "eCO2"],
      ["voc_level", "VOC Level (0-500 index)"],
      ["aqi_uba", "AQI (UBA)"],
      ["lux", "Ambient light"],
      ["fw", "Firmware"],
      ["transport", "Connection"],
    ]) {
      const value = h("dd", { text: "--" });
      this.advRows[key] = value;
      advGrid.append(h("dt", { text: caption }), value);
    }
    this.advGrid = advGrid;
    advGrid.style.display = "none";
    this.advBtn = h("button.linkbtn", {
      type: "button",
      text: "Advanced data  \u25b8",
      onclick: () => {
        const open = advGrid.style.display === "none";
        advGrid.style.display = open ? "" : "none";
        this.advBtn.textContent = `Advanced data  ${open ? "\u25be" : "\u25b8"}`;
      },
    });

    // --- history --------------------------------------------------------
    this.metricPicker = pillPicker(
      HISTORY_METRICS.map((m) => ({ value: m.key, label: m.label === "Temperature" ? "Temp" : m.label })),
      this.metric.key,
      (key) => {
        this.metric = HISTORY_METRICS.find((m) => m.key === key);
        this.refreshHistory(true);
      },
    );
    this.rangePicker = pillPicker(
      HISTORY_RANGES.map((r) => ({ value: r.seconds, label: r.key })),
      this.rangeSeconds,
      (seconds) => {
        this.rangeSeconds = seconds;
        this.refreshHistory(true);
      },
    );

    this.chartHost = h("div.chart-host");
    this.chartEmpty = h("div.chart-empty", { text: "No history for this range." });
    // A full pull is thousands of entries and takes a few seconds, which is
    // long enough that a bare empty chart reads as breakage.
    this.loadingDetail = h("div.chart-loading-sub", { text: "Reading history from the cube" });
    this.chartLoading = h(
      "div.chart-loading",
      h("div.spinner"),
      h("div.chart-loading-title", { text: "Fetching data" }),
      this.loadingDetail,
    );
    this.chartLoading.style.display = "none";
    this.chartReadout = h("span.faint", { text: "Hover the chart to read values" });
    this.chart = new HistoryChart(this.chartHost, (point) => this._onScrub(point));

    this.stats = ["Peak", "Average", "Lowest"].map((name) => {
      const value = h("div.value", { text: "--" });
      const when = h("div.when");
      return {
        name,
        value,
        when,
        el: h("div", h("div.caption", { text: name.toUpperCase() }), value, when),
      };
    });

    this.syncStatus = h("span.faint", { text: "No history synced yet" });
    this.syncBtn = h("button.btn", {
      type: "button",
      text: "Sync from device",
      onclick: () => this._syncNow(),
    });

    this.historyCard = h(
      "div.card.history-card",
      h(
        "div.history-header",
        h("h2.card-title", { text: "History" }),
        h("div.spacer"),
        this.metricPicker,
        this.rangePicker,
      ),
      this.chartReadout,
      h("div", { style: { position: "relative", flex: "1", minHeight: "0", display: "flex" } },
        this.chartHost,
        this.chartEmpty,
        this.chartLoading,
      ),
      h("div.stats-row", ...this.stats.map((s) => s.el)),
      h("div.row", this.syncStatus, h("div.spacer"), this.syncBtn),
    );

    // --- assembly -------------------------------------------------------
    this.el = h(
      "div.column.detail",
      h(
        "div.page-header",
        this.backBtn,
        this.title,
        this.typePill,
        this.connDot,
        this.connLabel,
        h("div.spacer"),
        this.menuBtn,
      ),
      this.syncBar,
      h(
        "div.detail-body",
        h(
          "div.detail-rail",
          this.hero,
          h("div.tiles", this.tiles.co2.el, this.tiles.voc.el, this.tiles.hum.el, this.tiles.temp.el),
          this.briCard,
          h("div.card", this.advBtn, advGrid),
        ),
        h("div.detail-main", this.historyCard),
      ),
    );
  }

  setDevice(device) {
    this.device = device;
    this.metric = HISTORY_METRICS[0];
    this.metricPicker.select(this.metric.key);
    this._historyVersion = -1;
    this.refresh();
    // Entering detail kicks off a sync, matching show_detail() in the tray.
    if (device.isConnected && !device.isSyncing && !device.slots.length) {
      device.syncHistory().catch(() => {});
    }
  }

  // ------------------------------------------------------------------ actions

  _syncNow() {
    this.device?.syncHistory().catch((err) => toast(err.message, "err"));
  }

  _openMenu() {
    const device = this.device;
    if (!device) return;
    openMenu(this.menuBtn, [
      { label: "Sync now", onSelect: () => this._syncNow() },
      { label: "Export CSV", onSelect: () => this._exportCsv() },
      "-",
      { label: "Rename", onSelect: () => this._rename() },
      { label: "Flash firmware", onSelect: () => this.onFlash(device) },
      ...(device.isPro
        ? [{ label: "Calibrate CO2 to 425 ppm", onSelect: () => this._runCo2Frc() }]
        : []),
      "-",
      { label: "Clear history on device", onSelect: () => this._clearHistory() },
      { label: "Disconnect", onSelect: () => this._disconnect() },
    ]);
  }

  async _rename() {
    const name = await promptDialog("Rename AirCube", "Name", this.device.name);
    if (name) {
      this.device.rename(name);
      this.refresh();
    }
  }

  async _runCo2Frc() {
    const ok = await confirmDialog(
      "Calibrate CO2 to 425 ppm",
      "Leave the cube in outdoor or open-window air for at least 10 minutes before continuing. This sets whatever it is measuring right now to 425 ppm.",
      "Calibrate",
    );
    if (!ok) return;
    try {
      const correction = await this.device.runCo2Frc();
      toast(`CO2 calibrated to 425 ppm (correction ${correction})`);
    } catch (err) {
      toast(err.message, "err");
    }
  }

  async _clearHistory() {
    const ok = await confirmDialog(
      "Clear history",
      `Erase the stored history on ${this.device.name}? The cube keeps recording, but the past readings are lost.`,
      "Clear history",
    );
    if (!ok) return;
    try {
      await this.device.clearHistory();
      toast("History cleared");
      this.refreshHistory();
    } catch (err) {
      toast(err.message, "err");
    }
  }

  async _disconnect() {
    try {
      await this.registry.forget(this.device);
      this.onBack();
    } catch (err) {
      toast(err.message, "err");
    }
  }

  _exportCsv() {
    const device = this.device;
    const slots = [...device.slots].sort((a, b) => a.timestamp - b.timestamp);
    if (!slots.length) {
      toast("Nothing to export yet. Sync the history first.", "err");
      return;
    }
    // Same columns as the tray's export, so the two are interchangeable.
    const header = [
      "timestamp", "sequence",
      "temp_avg_c", "temp_min_c", "temp_max_c",
      "hum_avg", "hum_min", "hum_max",
      "voc_avg", "voc_min", "voc_max",
      "co2_avg", "co2_min", "co2_max",
      "etvoc_avg", "etvoc_min", "etvoc_max",
    ];
    const rows = slots.map((s) =>
      [
        new Date(s.timestamp * 1000).toISOString(),
        s.sequence,
        s.tempAvg, s.tempMin, s.tempMax,
        s.humAvg, s.humMin, s.humMax,
        s.vocAvg, s.vocMin, s.vocMax,
        s.co2Avg, s.co2Min, s.co2Max,
        s.etvocAvg, s.etvocMin, s.etvocMax,
      ].join(","),
    );
    const safeName = device.name.replace(/[^\w.-]+/g, "-");
    downloadFile(`${safeName}-history.csv`, [header.join(","), ...rows].join("\n"));
    toast(`Exported ${slots.length} entries`);
  }

  // ------------------------------------------------------------------ refresh

  refresh() {
    if (!this.device) return;
    this.refreshLive();
    this.refreshSync();
    // Historical ranges redraw only after a sync. The live range updates for
    // every serial reading, using HistoryChart's in-place update path.
    const selectedRange = HISTORY_RANGES.find((r) => r.seconds === this.rangeSeconds);
    this.refreshHistory(selectedRange?.live || this.device.historyVersion !== this._historyVersion);
  }

  refreshLive() {
    const device = this.device;
    const reading = device.lastReading;
    const useF = prefs.get("useFahrenheit");

    this.title.textContent = device.name;
    this.typePill.textContent = device.modelLabel;

    const online = device.isOnline;
    this.connDot.className = online ? "dot tinted q-good" : "dot";
    this.connLabel.textContent = device.isConnected
      ? `${online ? "Connected" : "Waiting for data"} · ${updatedAgo(device.lastUpdated)}`
      : "Disconnected";

    if (!reading) {
      this.hero.className = "card hero q-none";
      this.heroTitle.textContent = "Waiting for data...";
      this.heroAdvice.textContent =
        "The cube sends a reading about once a second. Sensors need roughly three minutes to warm up after a power cycle.";
      this.heroPill.style.display = "none";
      // SVGElement.className is read-only, so classes go through setAttribute.
      this.gauge.el.setAttribute("class", "gauge q-none");
      this.gauge.clear();
      return;
    }

    const quality = readingQuality(reading);
    const cls = qualityClass(quality);
    this.hero.className = `card hero ${cls}`;
    this.heroTitle.textContent = qualityLabel(quality);
    this.heroAdvice.textContent = qualityAdvice(quality);
    this.heroPill.style.display = "";
    setPill(
      this.heroPill,
      quality === GOOD ? "No action needed" : "Ventilation recommended",
      cls,
    );
    this.gauge.el.setAttribute("class", `gauge ${cls}`);
    this.gauge.setScore(airScore(reading), qualityShortLabel(quality));

    // On Pro the CO2 tile shows the SCD41's true reading; on Base it falls
    // back to the ENS16x estimate, exactly as the tray does.
    const co2Value = device.isPro ? reading.co2 : reading.eco2;
    this.tiles.co2.caption.textContent = device.isPro ? "CO2" : "eCO2";
    this.tiles.co2.set(`${co2Value} ppm`, tileStatus("co2", co2Value));
    this.tiles.voc.set(`${reading.etvoc} ppb`, tileStatus("voc", reading.etvoc));
    this.tiles.hum.set(`${reading.humidity.toFixed(0)}%`, tileStatus("hum", reading.humidity));
    this.tiles.temp.set(
      useF
        ? `${cToF(reading.temperatureC).toFixed(1)}°F`
        : `${reading.temperatureC.toFixed(1)}°C`,
      tileStatus("temp", reading.temperatureC),
    );

    this.advRows.eco2.textContent = `${reading.eco2} ppm`;
    this.advRows.voc_level.textContent = String(reading.vocLevel);
    this.advRows.aqi_uba.textContent = reading.aqiUba ? String(reading.aqiUba) : "--";
    this.advRows.lux.textContent = device.isPro ? `${reading.lux.toFixed(0)} lx` : "n/a";
    // Reported on every frame since 2.0.3; before that it is only known if
    // this browser flashed the cube.
    this.advRows.fw.textContent = device.fwVersion
      ? `v${device.fwVersion}`
      : "not reported by this firmware";
    this.advRows.transport.textContent = "USB (Web Serial)";

    if (device.ledPercent != null) {
      this.briSlider.setValue(device.ledPercent);
      this.briValue.textContent = `${device.ledPercent}%`;
    }
    this.briSlider.disabled = !device.isConnected;
  }

  refreshHistory(force = true) {
    const device = this.device;
    if (!device) return;
    if (!force) {
      this._refreshSyncStatusText();
      return;
    }
    this._historyVersion = device.historyVersion;
    const useF = prefs.get("useFahrenheit");
    const metric = this.metric;
    const now = Date.now() / 1000;

    const floor = metricFloor(metric.key);
    const liveRange = HISTORY_RANGES.find((r) => r.seconds === this.rangeSeconds)?.live;
    const live = liveRange
      ? device
        .liveReadingsInRange(this.rangeSeconds)
        .map((reading) => ({
          time: reading.timestamp,
          value: liveMetricValue(metric.key, reading),
        }))
        .filter((point) => Number.isFinite(point.value) && point.value > floor)
      : [];
    const firstLiveTime = live.length ? live[0].time : Infinity;
    const validHistory = device
      .slotsInRange(this.rangeSeconds)
      .filter((s) => s.timestamp < firstLiveTime && slotValues(metric.key, s)[0] > floor);

    const convert = (v) => (metric.key === "temp" && useF ? cToF(v) : v);
    const unit =
      metric.key === "temp" ? (useF ? "°F" : "°C") : metric.unit;
    this.unit = unit;
    const format = (v) => `${v.toFixed(metric.decimals)}${unit.startsWith("°") ? unit : ` ${unit}`}`;
    this.formatValue = format;

    const segments = historySegments(validHistory).map((segment) =>
      segment.map((s) => [s.timestamp, convert(slotValues(metric.key, s)[0])]),
    );
    if (live.length) {
      segments.push(live.map((point) => [point.time, convert(point.value)]));
    }
    this.chart.setData(segments, `--m-${metric.key}`, format, [
      now - this.rangeSeconds,
      now,
    ]);
    const points = [
      ...validHistory.map((s) => ({
        value: convert(slotValues(metric.key, s)[0]),
        time: s.timestamp,
      })),
      ...live.map((point) => ({ value: convert(point.value), time: point.time })),
    ];
    this.chartEmpty.style.display = points.length || device.isSyncing ? "none" : "";

    // 2-hour sparklines on the tiles (24 slots of 5 minutes).
    const spark = device.sparklineSlots(24);
    this.tiles.co2.spark.setData(
      spark
        .map((s) => s.co2Avg)
        .filter((v) => v > CO2_VALID_FLOOR),
    );
    this.tiles.voc.spark.setData(spark.map((s) => s.etvocAvg).filter((v) => v > 0));
    this.tiles.hum.spark.setData(spark.map((s) => s.humAvg).filter((v) => v > 0));
    this.tiles.temp.spark.setData(
      spark.map((s) => s.tempAvg).filter((v) => v !== 0).map(convertTemp(useF)),
    );

    if (points.length) {
      const peak = points.reduce((a, b) => (b.value > a.value ? b : a));
      const low = points.reduce((a, b) => (b.value < a.value ? b : a));
      const average = points.reduce((sum, p) => sum + p.value, 0) / points.length;
      const rangeLabel = HISTORY_RANGES.find((r) => r.seconds === this.rangeSeconds)?.label ?? "";

      this.stats[0].value.textContent = format(peak.value);
      this.stats[0].when.textContent = formatDayTime(peak.time);
      this.stats[1].value.textContent = format(average);
      this.stats[1].when.textContent = rangeLabel;
      this.stats[2].value.textContent = format(low.value);
      this.stats[2].when.textContent = formatDayTime(low.time);
    } else {
      for (const stat of this.stats) {
        stat.value.textContent = "--";
        stat.when.textContent = "";
      }
    }

    this._refreshSyncStatusText();
  }

  _refreshSyncStatusText() {
    const device = this.device;
    if (device.isSyncing) {
      const { current, total } = device.syncProgress;
      this.syncStatus.textContent = total
        ? `Syncing ${current} of ${total} entries...`
        : "Syncing...";
    } else if (device.syncError) {
      this.syncStatus.textContent = `Sync failed: ${device.syncError}`;
    } else if (device.lastSyncedAt) {
      this.syncStatus.textContent = `${device.slots.length} entries · synced ${formatClock(device.lastSyncedAt)}`;
    } else {
      this.syncStatus.textContent = "No history synced yet";
    }
  }

  refreshSync() {
    const device = this.device;
    if (!device) return;
    this.syncBar.style.visibility = device.isSyncing ? "" : "hidden";
    this.chartLoading.style.display = device.isSyncing ? "" : "none";
    if (device.isSyncing) {
      const { current, total } = device.syncProgress;
      if (total > 0) this.syncBar.setFraction(current / total);
      else this.syncBar.setIndeterminate();
      this.loadingDetail.textContent = total
        ? `${current} of ${total} entries`
        : "Reading history from the cube";
      // Whatever the chart was showing, "no history" is the wrong message
      // while we are still fetching it.
      this.chartEmpty.style.display = "none";
    }
    this.syncBtn.disabled = device.isSyncing || !device.isConnected;
  }

  _onScrub(point) {
    if (!point) {
      this.chartReadout.textContent = "Hover the chart to read values";
      this.chartReadout.className = "faint";
      return;
    }
    this.chartReadout.textContent = `${this.formatValue(point.value)} · ${formatDayTime(point.time)}`;
    this.chartReadout.className = "muted";
  }
}

const convertTemp = (useF) => (celsius) => (useF ? cToF(celsius) : celsius);
