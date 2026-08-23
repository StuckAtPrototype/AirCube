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
import { LIVE_RANGE_SECONDS, seqDistance } from "./quality.js";

/** A cube counts as online while a reading has arrived within this window. */
const ONLINE_WINDOW_S = 15;
const NAMES_KEY = "aircube.names";
const FRC_DONE_KEY = "aircube.frcDone";

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

function loadFrcDone() {
  try {
    return JSON.parse(localStorage.getItem(FRC_DONE_KEY)) || [];
  } catch {
    return [];
  }
}

/**
 * Whether CO2 calibration has already been settled for the cube in this slot,
 * either by running it or by the user waving the prompt away.
 *
 * The cube cannot remember this for itself across an update: release images
 * are flashed at 0x0 and span the NVS partition at 0x9000, so every flash
 * erases the firmware's one-time prompt flag. The SCD41 holds its calibration
 * in the sensor, which a reflash does not touch, so this record is what stops
 * an already-calibrated cube asking again after each update.
 */
export function frcSettled(slot) {
  return Boolean(loadFrcDone()[slot]);
}

function markFrcSettled(slot) {
  const done = loadFrcDone();
  done[slot] = true;
  try {
    localStorage.setItem(FRC_DONE_KEY, JSON.stringify(done));
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
    this.frcNeeded = false;
    this.pendingFrcNudge = false;
    this._frcNudgeOffered = false;
    this.lastReading = null;
    this.liveReadings = [];
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

    this._wireLink();
  }

  _wireLink() {
    this.link.addEventListener("live", (e) => this._onLive(e.detail));
    this.link.addEventListener("config", (e) => this._onConfig(e.detail));
    this.link.addEventListener("closed", () => {
      this.isConnected = false;
      this._changed();
    });
  }

  /**
   * Adopt the port this cube came back on.
   *
   * A reset re-enumerates the USB interface, and because the cube reports no
   * serial number the browser cannot recognise it as the same device: it hands
   * back a fresh SerialPort rather than the original. Rebinding keeps the
   * cube's name, history and slot instead of listing it twice.
   */
  rebind(port) {
    this.port = port;
    this.link = new SerialLink(port);
    this._wireLink();
    this.isConnected = false;
    this._changed();
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
    this.liveReadings.push(reading);
    const cutoff = reading.timestamp - LIVE_RANGE_SECONDS;
    while (this.liveReadings.length && this.liveReadings[0].timestamp < cutoff) {
      this.liveReadings.shift();
    }
    this.isPro = reading.isPro;
    // Firmware older than 2.0.3 reports no version, so keep whatever the
    // flasher recorded for this cube rather than blanking it.
    if (reading.fwVersion) this.fwVersion = reading.fwVersion;
    this.frcNeeded = Boolean(reading.frcNeeded);
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

  /**
   * Forced recalibration of the Pro SCD41 to 425 ppm. The cube must already
   * have been sitting in that air for several minutes; this only issues the
   * command. Returns the signed correction word from the sensor.
   */
  async runCo2Frc() {
    if (!this.isPro) {
      throw new Error("CO2 calibration is only available on AirCube Pro");
    }
    if (!this.isConnected) {
      throw new Error("Cube is not connected");
    }
    const reply = await this.link.send(proto.cmdScd41Frc(), proto.isScd41FrcReply, {
      timeoutMs: 15000,
      retries: 0,
    });
    if (reply.status !== "ok") {
      throw new Error(reply.msg || "CO2 calibration failed");
    }
    this.frcNeeded = false;
    this.pendingFrcNudge = false;
    markFrcSettled(this.slot);
    return Math.round(Number(reply.correction) || 0);
  }

  /**
   * Clear the one-time post-upgrade calibration prompt without running FRC.
   * Remembered for this cube, so a later update does not ask again.
   */
  async dismissFrcNudge() {
    this.frcNeeded = false;
    this.pendingFrcNudge = false;
    markFrcSettled(this.slot);
    if (!this.isConnected) return;
    await this.link.send(proto.cmdScd41FrcAck(), proto.statusReply("scd41_frc_ack"));
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

  liveReadingsInRange(seconds = LIVE_RANGE_SECONDS) {
    const cutoff = Date.now() / 1000 - seconds;
    return this.liveReadings.filter((reading) => reading.timestamp >= cutoff);
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
  }

  get supported() {
    return "serial" in navigator;
  }

  async init() {
    if (!this.supported) return;
    const ports = await navigator.serial.getPorts();
    for (const port of ports) {
      // Each re-enumeration leaves another permission grant behind, since the
      // browser has no serial number to recognise a returning cube by. Those
      // dead grants are still handed back here, and adopting them is what
      // listed cubes that are not plugged in.
      if (port.connected === false) continue;
      await this._adopt(port);
    }

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
    // The user picked this port deliberately, so a refusal to open is worth
    // reporting rather than silently dropping.
    const device = await this._adopt(port, { reportFailure: true });
    this._changed();
    return device;
  }

  async _adopt(port, { reportFailure = false } = {}) {
    const existing = this.devices.find((d) => d.port === port);
    if (existing) {
      if (!existing.isConnected && !existing.heldForFlash) {
        await existing.connect().catch(() => {});
        this._changed();
      }
      return existing;
    }

    // A cube that just rebooted arrives as a different SerialPort object, so
    // give the new port to the one still waiting for it rather than listing a
    // second copy of the same cube.
    const returning = this.devices.find((d) => d.inFlashGrace && !d.isConnected);
    if (returning) {
      returning.rebind(port);
      const reconnected = await returning
        .connect()
        .then(() => true)
        .catch(() => false);
      if (reconnected) returning.endFlashGrace();
      this._changed();
      return returning;
    }

    // Only list a cube once its port actually opens. Because a cube carries no
    // serial number, every re-enumeration leaves behind another permission
    // grant, and all of them point at the same hardware: the first port opens
    // and the rest are refused. Listing the refusals is what put ghost cubes on
    // the home screen. A port the browser will not open is not a cube anyone
    // can use, whatever the reason.
    const device = new Device(port, this._nextSlot());
    try {
      await this._openWithRetry(device);
    } catch (err) {
      if (reportFailure) throw err;
      return null;
    }

    device.addEventListener("change", () => this._changed());
    this.devices.push(device);
    this._changed();
    return device;
  }

  /** A cube that has only just enumerated can refuse the first open. */
  async _openWithRetry(device, attempts = 3, delayMs = 350) {
    for (let attempt = 1; ; attempt++) {
      try {
        await device.connect();
        return;
      } catch (err) {
        if (attempt >= attempts) throw err;
        await new Promise((resolve) => setTimeout(resolve, delayMs));
      }
    }
  }

  /** Lowest unused slot, so forgetting a cube frees its name for the next one. */
  _nextSlot() {
    const used = new Set(this.devices.map((d) => d.slot));
    let slot = 0;
    while (used.has(slot)) slot++;
    return slot;
  }

  /**
   * Drop a cube for good.
   *
   * Closing the link is not enough: the browser keeps the port permission, so
   * getPorts() hands it back on the next load and init() adopts it again.
   * forget() revokes the grant, which means reconnecting needs a fresh trip
   * through the browser's device picker.
   */
  async forget(device) {
    await device.disconnect().catch(() => {});
    try {
      await device.port.forget?.();
    } catch {
      // Chrome before 103 has no SerialPort.forget(); the cube still goes
      // away for this session, it just comes back on the next load.
    }
    this.devices = this.devices.filter((d) => d !== device);
    this._changed();
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
