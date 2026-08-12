/* Device registry: the browser's answer to aircubeapp/manager.py.
 *
 * Web Serial exposes only the USB vendor and product IDs of a port, never the
 * serial number, and every AirCube reports the same 303A:1001. There is
 * therefore no stable per-cube identity across sessions: names are remembered
 * by the order cubes are first seen, and history lives in memory for the
 * session rather than in a per-device database.
 */

import { SerialLink } from "./serial.js";
import * as proto from "./protocol.js";
import { seqDistance } from "./quality.js";

/** A cube counts as online while a reading has arrived within this window. */
const ONLINE_WINDOW_S = 15;
const NAMES_KEY = "aircube.names";

let nextId = 1;

function loadNames() {
  try {
    return JSON.parse(localStorage.getItem(NAMES_KEY)) || [];
  } catch {
    return [];
  }
}

function saveName(slot, name) {
  const names = loadNames();
  names[slot] = name;
  try {
    localStorage.setItem(NAMES_KEY, JSON.stringify(names));
  } catch {
    /* private mode */
  }
}

export class Device extends EventTarget {
  constructor(port, slot) {
    super();
    this.id = `dev-${nextId++}`;
    this.slot = slot;
    this.port = port;
    this.link = new SerialLink(port);

    this.name = loadNames()[slot] || (slot === 0 ? "AirCube" : `AirCube ${slot + 1}`);
    this.isPro = false;
    this.fwVersion = "";
    this.lastReading = null;
    this.config = null;
    this.ledPercent = null;

    this.slots = [];
    this.historyWindowS = 300;
    // Bumped whenever the slot set changes, so views can skip redrawing the
    // history chart on every live reading.
    this.historyVersion = 0;
    this.lastSyncedAt = 0;
    this.isSyncing = false;
    this.syncProgress = { current: 0, total: 0 };
    this.syncError = "";

    this.isConnected = false;
    this.heldForFlash = false;
    // A flash ends with a hard reset, so the USB interface re-enumerates and
    // the browser fires a disconnect. Keep the cube listed through that window
    // instead of dropping it out from under the reconnect.
    this.flashGraceUntil = 0;

    this.link.addEventListener("live", (e) => this._onLive(e.detail));
    this.link.addEventListener("config", (e) => this._onConfig(e.detail));
    this.link.addEventListener("closed", () => {
      this.isConnected = false;
      this._changed();
    });
  }

  get isOnline() {
    return (
      this.isConnected &&
      this.lastReading != null &&
      Date.now() / 1000 - this.lastReading.timestamp < ONLINE_WINDOW_S
    );
  }

  get modelLabel() {
    return this.isPro ? "AirCube Pro" : "AirCube Base";
  }

  get lastUpdated() {
    return this.lastReading ? this.lastReading.timestamp : 0;
  }

  _changed() {
    this.dispatchEvent(new CustomEvent("change"));
  }

  _onLive(reading) {
    this.lastReading = reading;
    this.isPro = reading.isPro;
    this._changed();
  }

  _onConfig(config) {
    this.config = config;
    this.ledPercent = Math.round(config.intensity * 100);
    this._changed();
  }

  async connect() {
    if (this.isConnected || this.heldForFlash) return;
    await this.link.open();
    this.isConnected = true;
    this._changed();
    // Seed brightness and auto-dim state; a cube that is mid-boot may not
    // answer yet, and the periodic live stream will still work if it doesn't.
    this.refreshConfig().catch(() => {});
  }

  async disconnect() {
    await this.link.close();
    this.isConnected = false;
    this._changed();
  }

  async refreshConfig() {
    const reply = await this.link.send(proto.cmdGetConfig(), proto.isConfigReply);
    this._onConfig(proto.parseConfig(reply.config));
    return this.config;
  }

  async setBrightness(percent) {
    const pct = Math.max(0, Math.min(100, Math.round(percent)));
    this.ledPercent = pct;
    this._changed();
    await this.link.sendNoReply(proto.cmdSetIntensity(pct / 100));
  }

  async setAutoDim(options) {
    await this.link.sendNoReply(proto.cmdSetAutoDim(options));
    if (this.config) this.config.autoDim = { ...this.config.autoDim, ...options };
    this._changed();
  }

  async setReadoutPeriod(ms) {
    await this.link.sendNoReply(proto.cmdSetReadoutPeriod(ms));
    if (this.config) this.config.readoutPeriod = Math.round(ms);
    this._changed();
  }

  rename(name) {
    this.name = name.trim() || this.name;
    saveName(this.slot, this.name);
    this._changed();
  }

  // ------------------------------------------------------------------ history

  /**
   * Pull the whole history buffer a page at a time.
   *
   * The cube has no real-time clock, so slots carry only a wrapping sequence
   * number. We anchor the newest slot at the moment the sync completes and
   * space the rest backwards by the history window, the same approach the
   * tray uses.
   */
  async syncHistory() {
    if (this.isSyncing || !this.isConnected) return;
    this.isSyncing = true;
    this.syncError = "";
    this.syncProgress = { current: 0, total: 0 };
    this._changed();

    try {
      const infoReply = await this.link.send(
        proto.cmdGetHistoryInfo(),
        proto.isHistoryInfoReply,
      );
      const info = proto.parseHistoryInfo(infoReply.history_info);
      this.historyWindowS = info.windowS || 300;

      const total = Math.min(info.entries, info.capacity) || 0;
      this.syncProgress = { current: 0, total };
      this._changed();

      const collected = [];
      for (let start = 0; start < total; start += proto.HISTORY_PAGE) {
        if (!this.isConnected) throw new Error("Disconnected during sync");
        const count = Math.min(proto.HISTORY_PAGE, total - start);
        const page = await this.link.send(
          proto.cmdGetHistory(start, count),
          (d) => proto.isHistoryReply(d) && Number(d.start) === start,
          { timeoutMs: 5000 },
        );
        for (const raw of page.history) {
          const slot = proto.parseHistorySlot(raw);
          if (slot) collected.push(slot);
        }
        this.syncProgress = { current: Math.min(start + count, total), total };
        this._changed();
      }

      this._anchorAndStore(collected);
      this.lastSyncedAt = Date.now() / 1000;
    } catch (err) {
      this.syncError = err.message || String(err);
    } finally {
      this.isSyncing = false;
      this._changed();
    }
  }

  _anchorAndStore(slots) {
    this.historyVersion++;
    if (!slots.length) {
      this.slots = [];
      return;
    }
    const anchorTime = Date.now() / 1000;
    let newest = slots[0].sequence;
    for (const s of slots) {
      if (seqDistance(s.sequence, newest) < 0x8000) newest = s.sequence;
    }
    for (const s of slots) {
      s.timestamp = anchorTime - seqDistance(newest, s.sequence) * this.historyWindowS;
    }
    slots.sort((a, b) => a.timestamp - b.timestamp);
    this.slots = slots;
  }

  async clearHistory() {
    await this.link.sendNoReply(proto.cmdClearHistory());
    this.slots = [];
    this.historyVersion++;
    this.lastSyncedAt = 0;
    this._changed();
  }

  slotsInRange(seconds) {
    const cutoff = Date.now() / 1000 - seconds;
    return this.slots.filter((s) => s.timestamp >= cutoff);
  }

  /** Most recent n slots; 24 slots of 5 minutes is the tray's 2-hour spark. */
  sparklineSlots(n = 24) {
    return this.slots.slice(-n);
  }

  // -------------------------------------------------------------------- flash

  get inFlashGrace() {
    return this.heldForFlash || Date.now() < this.flashGraceUntil;
  }

  /** Close the data link so esptool-js can take the port, and stay closed. */
  async holdForFlash() {
    this.heldForFlash = true;
    this.flashGraceUntil = Date.now() + 30000;
    await this.link.close();
    this.isConnected = false;
    this._changed();
  }

  async releaseFlashHold() {
    this.heldForFlash = false;
    this._changed();
  }

  /** Called once the cube is back on the bus after a flash. */
  endFlashGrace() {
    this.flashGraceUntil = 0;
  }
}

export class DeviceRegistry extends EventTarget {
  constructor() {
    super();
    this.devices = [];
    this._slotCounter = 0;
  }

  get supported() {
    return "serial" in navigator;
  }

  async init() {
    if (!this.supported) return;
    const ports = await navigator.serial.getPorts();
    for (const port of ports) await this._adopt(port);

    navigator.serial.addEventListener("connect", (e) => {
      this._adopt(e.target).catch(() => {});
    });
    navigator.serial.addEventListener("disconnect", (e) => {
      const device = this.devices.find((d) => d.port === e.target);
      if (device) this._drop(device);
    });
    this._changed();
  }

  /** Prompt for a new cube. Chrome only lists ports the user hasn't granted. */
  async requestPort({ anyDevice = false } = {}) {
    const options = anyDevice ? {} : { filters: [proto.AIRCUBE_USB_FILTER] };
    const port = await navigator.serial.requestPort(options);
    const device = await this._adopt(port);
    this._changed();
    return device;
  }

  async _adopt(port) {
    const existing = this.devices.find((d) => d.port === port);
    if (existing) {
      if (!existing.isConnected && !existing.heldForFlash) {
        await existing.connect().catch(() => {});
        this._changed();
      }
      return existing;
    }

    const device = new Device(port, this._slotCounter++);
    device.addEventListener("change", () => this._changed());
    this.devices.push(device);
    try {
      await device.connect();
    } catch (err) {
      // A port already claimed by another tab or app stays listed but offline.
      device.syncError = err.message || String(err);
    }
    this._changed();
    return device;
  }

  _drop(device) {
    device.isConnected = false;
    if (!device.inFlashGrace) {
      this.devices = this.devices.filter((d) => d !== device);
    }
    this._changed();
  }

  byId(id) {
    return this.devices.find((d) => d.id === id) || null;
  }

  _changed() {
    this.dispatchEvent(new CustomEvent("change"));
  }
}
