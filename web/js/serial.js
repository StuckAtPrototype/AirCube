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

export class SerialLink extends EventTarget {
  constructor(port) {
    super();
    this.port = port;
    this.isOpen = false;
    this._reader = null;
    this._writer = null;
    this._pending = [];
    this._buffer = "";
    this._readLoop = null;
  }

  async open() {
    if (this.isOpen) return;
    await this.port.open({ baudRate: proto.SERIAL_BAUD });
    this.isOpen = true;
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

    try {
      await this.port.close();
    } catch {
      /* device may have been unplugged */
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
    if (!this._writer) this._writer = this.port.writable.getWriter();
    await this._writer.write(new TextEncoder().encode(text + "\n"));
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
        if (!this.isOpen) throw err;
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
