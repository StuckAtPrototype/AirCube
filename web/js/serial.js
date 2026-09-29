/* Web Serial transport for one AirCube.
 *
 * Browser analogue of aircubeapp/transports/serial_transport.py. The device
 * pushes a live JSON line roughly once a second, and command replies arrive
 * interleaved with that stream, so sends are matched to replies by shape
 * rather than by strict request/response ordering.
 *
 * The port is deliberately released cleanly on close(): esptool-js needs both
 * port.readable and port.writable unlocked before it can take over for a flash.
 */

import * as proto from "./protocol.js";

const DEFAULT_TIMEOUT_MS = 3000;
const MAX_LINE_BUFFER = 64 * 1024;

/**
 * How long a single write may take before the link is declared stalled.
 *
 * The ESP32-H2's USB Serial/JTAG receive FIFO holds 64 bytes. Firmware from
 * before February 2026 never reads it (it polled UART0 instead), so once the
 * host has pushed 64 bytes the device NAKs every further packet and the
 * browser's write promise never settles. A healthy cube drains a 30-byte
 * command in well under a millisecond, so anything approaching a second means
 * nobody is listening on the other end.
 */
export const WRITE_STALL_MS = 1500;
const CLOSE_TIMEOUT_MS = 4000;

export const STALLED_MESSAGE =
  "The AirCube is not accepting data over USB. Its firmware is too old to take " +
  "commands on this port; update the firmware to fix this. If flashing fails, " +
  "unplug the cube, plug it back in and try again.";

export class SerialLink extends EventTarget {
  constructor(port) {
    super();
    this.port = port;
    this.isOpen = false;
    /** Set once a write has hung; all later writes are refused immediately. */
    this.writeStalled = false;
    this._reader = null;
    this._writer = null;
    this._pending = [];
    this._buffer = "";
    this._readLoop = null;
  }

  async open() {
    if (this.isOpen) return;
    await this.port.open({ baudRate: proto.SERIAL_BAUD });
    // Chrome opens a port with DTR and RTS both asserted. That combination is
    // harmless, but dropping both on close passes through DTR=0/RTS=1, which
    // is the ESP32 auto-reset state and reboots the cube: sensor warm-up
    // restarts and the history window still accumulating in RAM is lost.
    // Parking RTS low keeps every later transition clear of it.
    try {
      await this.port.setSignals({ dataTerminalReady: true, requestToSend: false });
    } catch {
      /* platform or polyfill without signal control */
    }
    this.isOpen = true;
    this.writeStalled = false;
    this._buffer = "";
    this._readLoop = this._read();
  }

  /** Stop reading, drop pending requests and fully release the port. */
  async close() {
    if (!this.isOpen) return;
    this.isOpen = false;

    for (const p of this._pending.splice(0)) {
      clearTimeout(p.timer);
      p.reject(new Error("Serial link closed"));
    }

    if (this._reader) {
      try {
        await this._reader.cancel();
      } catch {
        /* already errored */
      }
      try {
        this._reader.releaseLock();
      } catch {
        /* already released */
      }
      this._reader = null;
    }
    if (this._writer) {
      try {
        this._writer.releaseLock();
      } catch {
        /* already released */
      }
      this._writer = null;
    }

    try {
      await this._readLoop;
    } catch {
      /* surfaced already */
    }
    this._readLoop = null;

    // A write that never completed (see WRITE_STALL_MS) is still queued in the
    // port's writable stream. Chrome aborts it when the port closes, but do
    // not bet the flasher on that: give up after a while so the caller can
    // tell the user to replug rather than sit on a spinner forever.
    try {
      await Promise.race([
        this.port.close(),
        new Promise((_, reject) =>
          setTimeout(() => reject(new Error("port.close() timed out")), CLOSE_TIMEOUT_MS),
        ),
      ]);
    } catch {
      /* device may have been unplugged, or the close is wedged */
    }
    this.dispatchEvent(new CustomEvent("closed"));
  }

  async _read() {
    const decoder = new TextDecoder();
    while (this.isOpen && this.port.readable) {
      this._reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this._reader.read();
          if (done) break;
          this._feed(decoder.decode(value, { stream: true }));
        }
      } catch (err) {
        if (this.isOpen) this.dispatchEvent(new CustomEvent("error", { detail: err }));
        break;
      } finally {
        try {
          this._reader.releaseLock();
        } catch {
          /* cancelled from close() */
        }
      }
      if (!this.isOpen) break;
    }
  }

  _feed(text) {
    this._buffer += text;
    if (this._buffer.length > MAX_LINE_BUFFER) {
      this._buffer = this._buffer.slice(-MAX_LINE_BUFFER);
    }
    let index;
    while ((index = this._buffer.indexOf("\n")) >= 0) {
      const line = this._buffer.slice(0, index).replace(/\r$/, "");
      this._buffer = this._buffer.slice(index + 1);
      if (line.trim()) this._handleLine(line);
    }
  }

  _handleLine(line) {
    this.dispatchEvent(new CustomEvent("line", { detail: line }));
    const data = proto.extractJson(line);
    if (!data) return;

    // A pending request wins over the generic handlers so that, say, a config
    // echo isn't mistaken for an unsolicited config push.
    const index = this._pending.findIndex((p) => p.matcher(data));
    if (index >= 0) {
      const [p] = this._pending.splice(index, 1);
      clearTimeout(p.timer);
      p.resolve(data);
      return;
    }

    if (proto.isLive(data)) {
      this.dispatchEvent(new CustomEvent("live", { detail: proto.parseLive(data) }));
    } else if (proto.isConfigReply(data)) {
      this.dispatchEvent(
        new CustomEvent("config", { detail: proto.parseConfig(data.config) }),
      );
    }
  }

  async write(text) {
    if (!this.isOpen || !this.port.writable) throw new Error("Serial link is closed");
    if (this.writeStalled) throw new Error(STALLED_MESSAGE);
    if (!this._writer) this._writer = this.port.writable.getWriter();

    const pending = this._writer.write(new TextEncoder().encode(text + "\n"));
    let timer;
    const stall = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(STALLED_MESSAGE)), WRITE_STALL_MS);
    });
    try {
      await Promise.race([pending, stall]);
    } catch (err) {
      if (err.message === STALLED_MESSAGE && this.isOpen) {
        this.writeStalled = true;
        // The hung write settles (or rejects) only when the port closes.
        pending.catch(() => {});
        this.dispatchEvent(new CustomEvent("stalled"));
      }
      throw err;
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * Send a command and wait for the reply its matcher accepts.
   * Retries once, since a line can be lost if the device is mid-reboot.
   */
  async send(command, matcher, { timeoutMs = DEFAULT_TIMEOUT_MS, retries = 1 } = {}) {
    let lastError;
    for (let attempt = 0; attempt <= retries; attempt++) {
      try {
        return await this._sendOnce(command, matcher, timeoutMs);
      } catch (err) {
        lastError = err;
        if (!this.isOpen || this.writeStalled) throw err;
      }
    }
    throw lastError;
  }

  _sendOnce(command, matcher, timeoutMs) {
    return new Promise((resolve, reject) => {
      const entry = { matcher, resolve, reject, timer: 0 };
      entry.timer = setTimeout(() => {
        const index = this._pending.indexOf(entry);
        if (index >= 0) this._pending.splice(index, 1);
        reject(new Error(`Timed out waiting for a reply to ${command}`));
      }, timeoutMs);
      this._pending.push(entry);
      this.write(command).catch((err) => {
        clearTimeout(entry.timer);
        const index = this._pending.indexOf(entry);
        if (index >= 0) this._pending.splice(index, 1);
        reject(err);
      });
    });
  }

  /** Fire-and-forget: the device answers with a status echo we don't need. */
  async sendNoReply(command) {
    await this.write(command);
  }
}

export const serialSupported = () => "serial" in navigator;
