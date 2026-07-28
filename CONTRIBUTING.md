# Contributing to AirCube

AirCube is fully open source -- firmware, hardware, desktop software, Home Assistant integration, and SmartThings Edge driver. Whether you want to fix a bug, add a feature, improve the docs, or port the desktop app to another platform, contributions are welcome.

This document covers everything you need to get the project building on your machine and understand how the code is organized.

---

## Quick Links

| Resource | Location |
|----------|----------|
| Customer-facing README | [README.md](README.md) |
| Home Assistant setup guide | [HOME_ASSISTANT.md](HOME_ASSISTANT.md) |
| SmartThings setup guide | [SMARTTHINGS.md](SMARTTHINGS.md) |
| Issue tracker | [GitHub Issues](https://github.com/StuckAtPrototype/AirCube/issues) |
| License | [Apache 2.0](LICENSE) |

---

## Project Layout

```
AirCube/
├── firmware/              # ESP-IDF firmware for the ESP32-H2
│   ├── CMakeLists.txt     # Top-level CMake (IDF project)
│   └── main/
│       ├── main.c                # App entry point, FreeRTOS tasks, LED loop
│       ├── device_model.c/h      # Base vs Pro detection (SCD41 / VCNL4040 presence)
│       ├── ens210.c/h            # ENS210 temperature & humidity driver (I2C)
│       ├── ens16x_driver.c/h     # ENS16X air quality driver (I2C)
│       ├── scd41.c/h             # SCD41 true NDIR CO2 driver (I2C, Pro only)
│       ├── vcnl4040.c/h          # VCNL4040 ambient light driver (I2C, Pro only)
│       ├── i2c_driver.c/h        # Shared I2C bus init
│       ├── led.c/h               # Thread-safe LED color & intensity control
│       ├── led_color_lib.c/h     # Hue-to-GRB color math
│       ├── ws2812_control.c/h    # Low-level WS2812 RMT driver
│       ├── button.c/h            # Button debounce & brightness cycling
│       ├── auto_dim.c/h          # Pro-only lux-based LED auto-dim
│       ├── serial_protocol.c/h   # JSON serial command interface (USB)
│       ├── history.c/h           # 7-day sensor history ring buffer on flash
│       ├── radio_mode.c/h        # BLE-vs-Zigbee mode selection, pairing/reboot logic
│       ├── ble_gatt.c/h          # BLE GATT server + BTHome v2 advertising
│       ├── ble_bthome.c/h        # Standalone BTHome broadcaster reference -- not in the build (absent from CMakeLists.txt SRCS)
│       ├── zigbee.c/h            # Zigbee End Device (ZCL + custom cluster + brightness)
│       └── environmental.c/h     # (placeholder / future use)
│
├── scripts/               # Python desktop tools
│   ├── aircube_app.py             # Full GUI app (PyQt/Matplotlib)
│   ├── aircube_logger.py          # Headless CSV logger
│   ├── aircube_data_visualizer.py # CSV live viewer (no serial)
│   ├── aircube_replay_script.py   # Replay logged CSV with timing
│   ├── build_exe.py               # PyInstaller build for desktop app
│   ├── aircube.spec               # PyInstaller spec
│   └── requirements.txt
│
├── kicad/                 # PCB design (KiCad)
│   ├── AirCube.kicad_pro/sch/pcb  # Schematic & layout
│   ├── gerbers/                    # Manufacturing files
│   └── AirCube v1.0 BOM.csv       # Bill of materials
│
├── mechanical/            # 3D-printable enclosure (STEP files)
│
├── zha/                   # Home Assistant ZHA quirk
│   └── aircube.py
│
├── z2m/                   # Zigbee2MQTT external converter
│   └── aircube.js
│
├── smartthings/           # Samsung SmartThings Edge driver (Zigbee hub)
│   ├── README.md
│   ├── driver-channel.json
│   └── aircube-zigbee/    # Driver package (config, fingerprints, profile, Lua)
│       ├── config.yml
│       ├── fingerprints.yml
│       ├── profiles/
│       └── src/
│
├── README.md              # Customer-facing product page
├── HOME_ASSISTANT.md      # Home Assistant integration guide
├── SMARTTHINGS.md         # SmartThings hub + CLI integration guide
├── CONTRIBUTING.md        # This file
└── LICENSE                # Apache 2.0
```

---

## Setting Up the Firmware

### Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) **v5.0 or later** (v5.3+ recommended)
- A USB-C cable (data-capable)
- An AirCube board, or any ESP32-H2 dev board with ENS210 + ENS16X on I2C

### Clone and build

```bash
git clone https://github.com/StuckAtPrototype/AirCube.git
cd AirCube/firmware

# Set the chip target (only needed once)
idf.py set-target esp32h2

# Build
idf.py build

# Flash and open serial monitor
idf.py -p COM3 flash monitor    # Windows -- replace COM3 with your port
idf.py -p /dev/ttyUSB0 flash monitor   # Linux
```

Press `Ctrl+]` to exit the IDF monitor.

### Common build issues

| Problem | Fix |
|---------|-----|
| `idf.py` not found | Run the IDF export script first (`export.bat` on Windows, `. ./export.sh` on Linux/macOS) |
| Wrong chip target | Run `idf.py set-target esp32h2` and rebuild |
| Stale build artifacts | `idf.py fullclean` then `idf.py build` |
| Flash fails | Hold BOOT, press RESET, release BOOT to enter download mode |

---

## Firmware Architecture

### Tasks and main loop

`app_main()` in `main.c` runs the initialization sequence, then spawns two FreeRTOS tasks and enters the LED update loop:

```
app_main()
  ├── Init: NVS, I2C, serial, LED, history, button
  ├── device_model_detect()          -- Base vs Pro, from SCD41 / VCNL4040 presence
  ├── ENS210 + ENS16X init (always); SCD41 + VCNL4040 init (Pro only)
  ├── radio_mode_init()              -- reads NVS join/pairing flags, picks boot mode
  │     ├── BLE mode (default)  ──► ble_gatt_init()   -- GATT server + BTHome advertising
  │     └── Zigbee mode         ──► zigbee_init()     -- only if previously joined, or a pairing request is pending
  ├── xTaskCreate(sensor_task)    -- reads sensors, sends JSON, logs history, pushes to the active radio
  ├── xTaskCreate(command_task)   -- polls for incoming serial commands
  └── Main loop (20ms tick)       -- smooth LED color transitions based on display_level (see below)
```

Exactly one of BLE or Zigbee runs per boot -- the ESP32-H2 has a single radio, and running both
concurrently isn't supported. See "Pairing behavior" below for how the firmware switches between them.

### Data flow

```
ENS210 (I2C)  ──► sensor_task ──► serial JSON output (USB)
ENS16X (I2C)  ──►      │       ├─► history_record_sample() ──► flash ring buffer
SCD41 (I2C, Pro) ──►    │       └─► active radio:
VCNL4040 (I2C, Pro) ──► │             ├─► zigbee_update_sensors()  ──► Zigbee attribute reports (every 10s), only in Zigbee mode
                        │             └─► ble_gatt live-data notify ──► BLE Live Data char. / BTHome advertising, only in BLE mode
                        │
     VOC Level, CO2 Level (Pro) ──► main loop ──► display_level = max(VOC Level, CO2 Level on Pro) ──► LED color (green-to-red)
                                                    ▲
Home Assistant / SmartThings ──► Zigbee Analog Output write ──► led_set_intensity() (brightness)
```

### Module overview

**Sensors**

- `device_model.c` -- Detects Base vs Pro hardware via `aircube_model_detect()`: Pro if either the SCD41 or VCNL4040 responds on I2C. `aircube_model_is_pro()` / `aircube_model_name()` ("base"/"pro") are used throughout the firmware (LED arbitration, Zigbee cluster gating, serial JSON, BLE device info).
- `ens210.c` -- I2C driver for the ENS210 temperature/humidity sensor. Exposes `ens210_get_temperature()`, `ens210_get_humidity()`.
- `ens16x_driver.c` -- I2C driver for the ENS16X air quality sensor. Reads eTVOC, eCO2, VOC Level, and AQI-UBA. Accepts environmental compensation data from the ENS210. `ens16x_read_aqi()` (AQI-S) is deprecated and always returns 0 -- kept only for serial JSON compatibility.
- `scd41.c` -- I2C driver for the Sensirion SCD41 (Pro only). Provides true NDIR CO2 in ppm, plus its own temperature/humidity readings. Used for both the LED's CO2 Level and, on Pro, the history/BLE/serial CO2 field.
- `vcnl4040.c` -- I2C driver for the Vishay VCNL4040 ambient light sensor (Pro only). Feeds `auto_dim.c` for automatic night-time LED dimming.

**LED**

- `led.c` -- Thread-safe color and intensity control for WS2812 LEDs. Uses a mutex so any task can call `led_set_color()` / `led_set_intensity()`.
- `led_color_lib.c` -- Converts a 16-bit hue to a 24-bit GRB value via `get_color_from_hue()`.
- `ws2812_control.c` -- Low-level RMT peripheral driver for WS2812 timing.

**Communication**

- `serial_protocol.c` -- JSON-over-USB serial interface. Sends periodic sensor data, accepts commands (see Serial Protocol below).
- `radio_mode.c` -- Chooses BLE or Zigbee at boot and manages the transition between them. Default boot mode is **BLE**, unless NVS records the device as already Zigbee-joined or a pairing request is pending. A long button press while in BLE mode sets an NVS pairing flag and reboots (`esp_restart()`) into Zigbee mode, where network steering begins and consumes the flag; a long press while already in Zigbee mode starts steering directly, with no reboot. `radio_mode_revert_to_ble()` clears the NVS flags and reboots back to BLE if steering fails/times out on a factory-new device, or if the device is removed from its Zigbee network.
- `ble_gatt.c` -- BLE GATT server (service UUID `A17C0DE0-...`: Device Info, Live Data, History Request/Data, Brightness) plus inline BTHome v2 advertising, active only while the device is in BLE mode. See [`docs/BLE_GATT_PROTOCOL.md`](docs/BLE_GATT_PROTOCOL.md) for the full protocol.
- `zigbee.c` -- Registers a Zigbee End Device on the ESP32-H2's native 802.15.4 radio, active only while the device is in Zigbee mode. Exposes temperature/humidity via standard ZCL clusters, eCO2/eTVOC/VOC Level via custom cluster 0xFC01, LED brightness via the standard Analog Output cluster (0x000D), and on Pro hardware, true CO2 and illuminance via standard clusters not yet exposed by any integration (see "Zigbee Integration" below).

**Storage**

- `history.c` -- Append-only ring buffer on a dedicated flash partition. Accumulates sensor samples in RAM and flushes a min/avg/max summary every 5 minutes. Stores up to 7 days (2016 entries). Each slot is exactly 32 bytes.

**Input**

- `button.c` -- GPIO debounce with short press (brightness cycle) and long press (radio pairing, via `radio_mode_start_pairing()`).

---

## Serial Protocol Reference

The AirCube communicates over USB-Serial-JTAG at **115200 baud**. All messages are single-line JSON terminated by `\n`.

### Device output (sent every readout period, default 1s)

```json
{
  "model": "pro",
  "ens210": {"status": 0, "temperature_c": 23.45, "temperature_f": 74.21, "humidity": 52.30},
  "ens16x": {"status": "OK", "etvoc": 42, "eco2": 415, "aqi": 3, "aqi_s": 0, "aqi_uba": 1},
  "scd41": {"co2": 512},
  "vcnl4040": {"lux": 84.2},
  "timestamp": 12345
}
```

`model` is `"base"` or `"pro"`. `aqi_s` is the deprecated AQI-S score -- `ens16x_read_aqi()` always
returns `0` now; the field is kept only for serial JSON compatibility. `scd41.co2` and
`vcnl4040.lux` are `0` on Base hardware (no SCD41/VCNL4040 fitted) and real readings on Pro.
`timestamp` is milliseconds since boot.

### Commands (send to device)

All commands are JSON with a `"cmd"` field. Send a complete JSON object followed by `\n`.

| Command | Payload | Response |
|---------|---------|----------|
| `get_config` | `{"cmd":"get_config"}` | `{"config":{"intensity":0.60,"readout_period":1000,"auto_dim":{...}}}` |
| `set_intensity` | `{"cmd":"set_intensity","value":0.3}` | `{"status":"ok","cmd":"set_intensity","value":0.30}` |
| `set_auto_dim` | `{"cmd":"set_auto_dim","enabled":true,"night_enter_lux":5,"day_exit_lux":15,"night_dim_pct":10}` | `{"config":{...}}` (full config echo) |
| `set_readout_period` | `{"cmd":"set_readout_period","value":500}` | `{"status":"ok","cmd":"set_readout_period","value":500.00}` |
| `get_history_info` | `{"cmd":"get_history_info"}` | `{"history_info":{"entries":288,"capacity":2016,"slot_bytes":32,"window_us":300000000}}` |
| `get_history` | `{"cmd":"get_history","start":0,"count":48}` | `{"history":[...],"start":0,"count":48}` |
| `clear_history` | `{"cmd":"clear_history"}` | `{"status":"ok","cmd":"clear_history","value":0.00}` |

**Shortcut:** Typing just `h` in the serial monitor dumps the entire history as CSV.

### Intensity range

`set_intensity` accepts `0.0` (off) to `1.0` (full brightness). It persists the **configured** brightness; on Pro hardware `auto_dim` may lower the **effective** LED output at night without changing the stored value.

### Auto-dim (Pro only)

Lux-based night dimming uses the VCNL4040 ambient reading with hysteresis (default: enter night below 5 lux, exit day above 15 lux). Base hardware disables auto-dim automatically.

Button / HA brightness presets map to night policy:

| Configured % | Preset | Day | Night (auto-dim on) |
|---|---|---|---|
| 0 | Off | Off | Off |
| 1–10 | 10% | Configured | Off |
| 11–30 | 30% | Configured | Off |
| 31–60 | 60% | Configured | Off |
| 61–100 | 100% | Configured | 10% (default `night_dim_pct`) |

BLE/Zigbee report the configured brightness, not the auto-dimmed effective value.

### Readout period range

`set_readout_period` accepts `100` to `10000` (milliseconds).

### History slot format

Each history entry contains min/avg/max for all five measurements over one 5-minute window. Temperature and humidity are stored as `int16 x 100` (e.g., 2345 = 23.45 C). VOC Level, eCO2, and eTVOC are raw uint16 values.

Abbreviated JSON keys in `get_history` responses:

| Key | Meaning |
|-----|---------|
| `seq` | Sequence number |
| `t_a`, `t_n`, `t_x` | Temperature avg, min, max (x100 C) |
| `h_a`, `h_n`, `h_x` | Humidity avg, min, max (x100 %) |
| `q_a`, `q_n`, `q_x` | VOC Level avg, min, max |
| `c_a`, `c_n`, `c_x` | CO2 avg, min, max (ppm) -- **true CO2 (SCD41) on Pro, eCO2 (ENS16X estimate) on Base** |
| `v_a`, `v_n`, `v_x` | eTVOC avg, min, max (ppb) |

---

## Zigbee Integration

The ESP32-H2 has a native IEEE 802.15.4 radio. AirCube registers as a Zigbee End Device with the following clusters on **endpoint 10**:

| Cluster | ID | Attributes |
|---------|----|-----------|
| Temperature Measurement | 0x0402 | `measuredValue` (int16, x100 C) |
| Relative Humidity | 0x0405 | `measuredValue` (uint16, x100 %) |
| Custom Air Quality | 0xFC01 | `eco2` (0x0000), `etvoc` (0x0001), `aqi` (0x0002) -- all uint16, read-only |
| Analog Output | 0x000D | `presentValue` (float, 0--100) -- LED brightness, writable |
| Carbon Dioxide Measurement | 0x040D | `measuredValue` (float, ppm) -- **Pro only**, true CO2 from the SCD41 |
| Illuminance Measurement | 0x0400 | `measuredValue` (uint16, lux) -- **Pro only**, from the VCNL4040 |

The custom cluster requires a **ZHA quirk** or **Zigbee2MQTT external converter** on the Home Assistant side. Both are included in the repo (`zha/aircube.py` and `z2m/aircube.js`). On a **Samsung SmartThings** hub, use the Edge driver in `smartthings/aircube-zigbee/` and follow [SMARTTHINGS.md](SMARTTHINGS.md).

**Pro's CO2 (0x040D) and illuminance (0x0400) clusters are firmware-only today.** They're declared
on the Zigbee endpoint, but the ZHA quirk, Z2M converter, and SmartThings Edge driver in this repo
do not currently read them, so they won't appear as entities on any hub yet. Contributions to wire
these up in the integrations are welcome.

See [HOME_ASSISTANT.md](HOME_ASSISTANT.md) for Home Assistant setup instructions.

### Pairing behavior

The ESP32-H2 runs exactly one radio stack at a time -- BLE or Zigbee, never both (see "Tasks and
main loop" above). The device boots into **BLE mode by default**, unless it's already joined to a
Zigbee network (or a pairing request is pending) per the NVS flags read at boot.

- A 3-second button hold triggers pairing mode at any time (`radio_mode_start_pairing()`).
  - If the device is currently in **BLE mode**, this sets an NVS pairing flag and calls
    `esp_restart()`, rebooting into Zigbee mode, where network steering begins immediately and
    consumes the flag.
  - If the device is already in **Zigbee mode** (previously joined), this starts network steering
    directly -- no reboot.
- During Zigbee pairing/steering, the LED flashes blue at 2 Hz.
- Pairing mode times out after 60 seconds if no network is found.
- If steering fails or times out on a factory-new device, or the device is later removed from its
  Zigbee network, `radio_mode_revert_to_ble()` clears the NVS flags and reboots back into BLE mode.

## BLE Integration

When not joined to a Zigbee network, AirCube runs a BLE GATT server (`ble_gatt.c`) plus BTHome v2
advertising, so it works with the AirCube apps or a Home Assistant Bluetooth proxy with no pairing
step at all. See [`docs/BLE_GATT_PROTOCOL.md`](docs/BLE_GATT_PROTOCOL.md) for the full protocol
(service/characteristic UUIDs, live data layout, history sync).

---

## Desktop Apps (scripts/)

### Prerequisites

```bash
cd scripts
pip install -r requirements.txt
```

### aircube_app.py -- Full desktop GUI

Live sensor display, color-coded VOC Level, three-panel charts (temp/humidity, VOC Level, gas levels), optional CSV logging, configurable history depth (50--1000 points).

```bash
python aircube_app.py
```

### AirCube Tray -- system tray monitor (separate repo)

For a minimal Windows taskbar-only view of VOC Level, see the companion [**AirCubeTray** repo](https://github.com/StuckAtPrototype/AirCubeTray). It ships its own installer and auto-detects the AirCube over USB.

### Other scripts

| Script | Purpose |
|--------|---------|
| `aircube_logger.py` | Headless CSV logger (no GUI) |
| `aircube_data_visualizer.py` | Live plots from a CSV file (no serial) |
| `aircube_replay_script.py` | Replay a logged CSV with original timing |

### Building standalone executables

```bash
pip install pyinstaller

python build_exe.py     # Produces dist/AirCube.exe
```

The standalone tray app build lives in its own repo: [AirCubeTray](https://github.com/StuckAtPrototype/AirCubeTray).

---

## Hardware

### Components

| Part | Description |
|------|------------|
| ESP32-H2-MINI-1 | MCU with 802.15.4 (Zigbee/Thread) radio |
| ENS210 | Temperature and humidity sensor (I2C) |
| ENS161 / ENS16X | Air quality sensor -- eTVOC, eCO2, VOC Level (I2C) |
| WS2812 x3 | RGB LEDs |
| USB-C connector | Power and data |
| Tactile button | Brightness control and Zigbee pairing |

### PCB

KiCad project files are in `kicad/`. Includes schematic, layout, Gerber files for manufacturing, and a BOM CSV.

### Enclosure

3D-printable STEP files in `mechanical/`. Top and bottom halves snap together.

---

## How to Contribute

### Reporting bugs

Open a [GitHub Issue](https://github.com/StuckAtPrototype/AirCube/issues) with:
- What you expected vs. what happened
- Steps to reproduce
- Firmware version / commit hash
- Serial monitor output if relevant

### Submitting changes

1. Fork the repo and create a branch from `master`.
2. Make your changes. Keep commits focused -- one logical change per commit.
3. Test on hardware if you're changing firmware. Test the desktop app if you're changing scripts.
4. Open a pull request against `master`. Describe what you changed and why.

### Code style

- **Firmware (C):** Follow the existing style -- 4-space indentation, `snake_case` for functions and variables, `UPPER_CASE` for defines and constants. Use ESP-IDF logging macros (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`).
- **Python scripts:** Standard Python conventions. No strict formatter enforced, but keep it readable.
- **Commit messages:** Short summary line, imperative mood (e.g., "Add history CSV export command").

### Ideas for contributions

- **SmartThings** -- driver improvements; WWST certification with Samsung ([certification overview](https://developer.smartthings.com/docs/certification/overview))
- **New sensor support** -- PM2.5, CO, noise level
- **Web dashboard** -- local web server on the ESP32-H2 or a companion app
- **macOS / Linux tray app** -- the current tray app is Windows-only
- **Matter support** -- the ESP32-H2 supports Matter over Thread
- **Power optimization** -- light sleep between sensor reads
- **OTA firmware updates** -- over Zigbee or USB
- **Additional languages** -- translations for the desktop app
- **Unit tests** -- for the history module, serial parser, color math

---

## License

Apache License 2.0. See [LICENSE](LICENSE) for the full text. Contributions are made under the same license.
