# AirCube Web Serial Monitor

`aircube_webapp.html` is a browser-based replacement for the `aircube_app.py`
desktop app. It reads the AirCube's live sensor stream over the **Web Serial
API** and shows current values, scrolling charts, CSV logging, and device
controls -- all from a single self-contained HTML file (no build step, no
external libraries, works offline).

It exists because the Python desktop app **cannot run on a Chromebook**. The
sections below explain why, and document the one non-obvious detail (the DTR/RTS
handshake) that makes reading work.

---

## Quick start

1. **Un-share the device from Linux.** In ChromeOS Settings -> About ChromeOS ->
   Linux -> USB preferences, make sure **"USB JTAG/serial debug unit"** is
   **OFF** (not shared into Crostini). Web Serial reads through the ChromeOS host
   driver, so the device must stay with the host.
2. **Serve the page over a secure context.** Web Serial only works over `https`
   or `http://localhost`. The simplest local option:
   ```bash
   cd scripts
   python3 -m http.server 8000
   ```
   If `http://localhost:8000` is not reachable from the ChromeOS browser, add a
   port-forward for TCP 8000 in ChromeOS Settings -> About ChromeOS -> Linux ->
   Port forwarding.
3. **Open it in Chrome** (or Edge): `http://localhost:8000/aircube_webapp.html`
4. Click **Connect**, choose the AirCube in the picker, and live data appears.

Opening the file directly with a `file://` URL will **not** work -- that is not a
secure context and Web Serial is disabled there.

---

## Features

Faithful to `aircube_app.py`:

- **Live value cards**: Temperature, Humidity, AQI, eCO2, eTVOC. AQI is color
  coded with the same bands as the desktop app and the device LED gradient:
  green (<=50), yellow (<=100), orange (<=200), red (>200).
- **Three scrolling charts** (canvas, no external chart library):
  Temperature + Humidity, AQI, and eCO2 + eTVOC, with the x-axis in seconds
  since the first sample.
- **History window** control (default 300 points) limits how much the charts
  show.
- **CSV logging** with the identical 9-column header
  (`timestamp, ens210_status, temperature_c, temperature_f, humidity,
  ens16x_status, etvoc, eco2, aqi`):
  - **Download CSV (session)** exports everything captured since connecting.
  - **Start file log...** streams each sample to a chosen file via Chrome's
    File System Access API (data is committed when you Stop the log).
- **Sample count** and **connection status** in the header.

Added on top of the desktop app (these use firmware commands the Python app
never exposed):

- **Temperature unit toggle** (C / F) -- updates the card and the chart live.
- **Field definition tooltips** -- hover any value label for a plain-language
  definition.
- **Device controls**:
  - **Get config** -> reads current LED intensity and readout period.
  - **LED intensity** slider -> `set_intensity` (0.0 - 1.0).
  - **Readout period** -> `set_readout_period` (100 - 10000 ms).
  - **Request history dump (h)** -> dumps stored history as CSV into the log.

---

## Why the Python app does not work on a Chromebook

This took a full debugging session to pin down. The findings, in order:

The AirCube is an **ESP32-H2** using the chip's **native USB Serial/JTAG**
peripheral (USB id **`0x303A:0x1001`**). It enumerates as a *vendor-specific*
(class `0xFF`) device with three interfaces:

| Interface | Endpoints           | Purpose                          |
|-----------|---------------------|----------------------------------|
| 0         | interrupt IN `0x82` | CDC comm / control               |
| 1         | bulk OUT `0x01`, IN `0x81` | serial console stream (the JSON) |
| 2         | bulk OUT/IN         | JTAG                             |

What we tried, and what happened:

1. **pyserial (what `aircube_app.py` uses) sees nothing.** The Crostini
   (termina) VM kernel has **no `cdc-acm` module** (`modprobe cdc-acm` fails),
   so no `/dev/ttyACM*` node is ever created. `serial.tools.list_ports.comports()`
   returns an empty list. (Reproduce with `test_serial_discovery.py`.)

2. **pyusb in Crostini can open the device but cannot read.** It discovers and
   opens `0x303A:0x1001` and finds endpoint `0x81`, but the device gates its TX
   until the host marks the port "connected", and the required control transfer
   (`SET_CONTROL_LINE_STATE`, a class interface-OUT request) **times out** through
   ChromeOS/Crostini's USB passthrough. Class-IN requests (`GET_LINE_CODING`) and
   standard control-OUT (`set_configuration`) work -- only class control-OUT is
   dropped. (Reproduce with `test_pyusb_discovery.py`.)

3. **WebUSB in the browser cannot claim the interface.** The ChromeOS *host*
   kernel's `cdc-acm` driver owns the interface (`claimInterface` fails with
   "Unable to claim interface"). That failure is the clue that the host exposes
   the device as a serial port.

4. **Web Serial works** -- it reads *through* the host `cdc-acm` driver instead
   of fighting it, and it can assert DTR/RTS natively via `setSignals()`.

### The key detail: DTR = 0, RTS = 1

The ESP32-H2 USB Serial/JTAG only streams once the host marks the port
"connected". The combination that works is:

```js
port.setSignals({ dataTerminalReady: false, requestToSend: true });  // DTR=0, RTS=1
```

Setting **DTR = 1 resets/halts** the chip (the USB Serial/JTAG ROM interprets
certain DTR/RTS states as a reset-into-download-mode request), which produces
total silence. This was the root cause of a long "why is there no data" hunt:
every earlier attempt asserted DTR=1 and got nothing. The desktop app works on a
normal laptop only because the laptop's `cdc-acm` driver handles this for it.

The firmware streams sensor JSON unconditionally about once per second
(`serial_send_sensor_data` in `firmware/main/serial_protocol.c`), so once the
signal handshake is right, data flows immediately.

---

## Serial protocol reference

**Sensor stream** (one newline-terminated JSON object per readout period):

```json
{"ens210":{"status":0,"temperature_c":22.50,"temperature_f":72.50,"humidity":45.00},
 "ens16x":{"status":"OK","etvoc":120,"eco2":650,"aqi":95,"aqi_s":100,"aqi_uba":2},
 "timestamp":123456}
```
`timestamp` is milliseconds since boot. `aqi` is the canonical TVOC-derived AQI
(0-500). `aqi_s` (ScioSense relative score) and `aqi_uba` are USB-serial only.

**Commands** (send as a newline-terminated line):

| Command | Effect | Response |
|---------|--------|----------|
| `{"cmd":"get_config"}` | read settings | `{"config":{"intensity":f,"readout_period":ms}}` |
| `{"cmd":"set_intensity","value":0.0-1.0}` | LED brightness | `{"status":"ok","cmd":"set_intensity","value":f}` |
| `{"cmd":"set_readout_period","value":100-10000}` | sample period (ms) | `{"status":"ok",...}` |
| `{"cmd":"get_history_info"}` | history metadata | `{"history_info":{...}}` |
| `{"cmd":"get_history","start":n,"count":n}` | history page | history rows |
| `{"cmd":"clear_history"}` | erase stored history | status |
| `h` | dump full history as CSV | CSV text |

---

## Browser requirements

- **Chrome or Edge** (Web Serial is not in Firefox or Safari).
- A **secure context**: `https://` or `http://localhost`.
- **File System Access API** (Chrome/Edge) is needed for "Start file log...";
  "Download CSV" works everywhere Web Serial does.

---

## Troubleshooting

- **Device not in the Connect picker** -> it is still shared into Linux (Crostini)
  or held by another tab/app. Un-share it (Quick start step 1) and close other
  connections.
- **Connects but no data / byte counter stays at 0** -> the signal handshake is
  wrong for your situation. The app uses DTR=0 RTS=1; if you forked it, do not
  assert DTR=1 (it resets the chip). The standalone `webserial_aircube_test.html`
  has manual signal buttons for experimenting.
- **"Web Serial unavailable"** -> you opened the file over `file://` or plain
  `http`. Serve it over `localhost` or `https`.
- **Garbled characters** -> unrelated terminal/font issue; does not affect the
  web app.

---

## Files in this folder

| File | Purpose |
|------|---------|
| `aircube_webapp.html` | The full web app (this README documents it). |
| `webserial_aircube_test.html` | Minimal Web Serial signal tester (DTR/RTS buttons, byte counter). |
| `webusb_aircube_test.html` | Minimal WebUSB probe (shows why WebUSB cannot claim the interface). |
| `test_serial_discovery.py` | Proves pyserial sees no ports in Crostini. |
| `test_pyusb_discovery.py` | Proves pyusb can open but not read (DTR control-OUT times out). |
| `aircube_app.py` | Original PyQt6 desktop app (works on a regular laptop, not a Chromebook). |

---

## License

Apache License 2.0. See the repository `LICENSE` file. The web app carries the
standard Apache 2.0 header at the top of `aircube_webapp.html`.
