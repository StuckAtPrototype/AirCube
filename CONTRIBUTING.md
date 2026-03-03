# Contributing to AirCube

AirCube is fully open source -- firmware, hardware, desktop software, and Home Assistant integration. Whether you want to fix a bug, add a feature, improve the docs, or port the desktop app to another platform, contributions are welcome.

This document covers everything you need to get the project building on your machine and understand how the code is organized.

---

## Quick Links

| Resource | Location |
|----------|----------|
| Customer-facing README | [README.md](README.md) |
| Home Assistant setup guide | [HOME_ASSISTANT.md](HOME_ASSISTANT.md) |
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
│       ├── ens210.c/h            # ENS210 temperature & humidity driver (I2C)
│       ├── ens16x_driver.c/h     # ENS16X air quality driver (I2C)
│       ├── i2c_driver.c/h        # Shared I2C bus init
│       ├── led.c/h               # Thread-safe LED color & intensity control
│       ├── led_color_lib.c/h     # Hue-to-GRB color math
│       ├── ws2812_control.c/h    # Low-level WS2812 RMT driver
│       ├── button.c/h            # Button debounce & brightness cycling
│       ├── serial_protocol.c/h   # JSON serial command interface (USB)
│       ├── history.c/h           # 7-day sensor history ring buffer on flash
│       ├── zigbee.c/h            # Zigbee End Device (ZCL + custom cluster)
│       └── environmental.c/h     # (placeholder / future use)
│
├── scripts/               # Python desktop tools
│   ├── aircube_app.py             # Full GUI app (PyQt/Matplotlib)
│   ├── aircube_tray.py            # Windows system tray AQI monitor
│   ├── aircube_logger.py          # Headless CSV logger
│   ├── aircube_data_visualizer.py # CSV live viewer (no serial)
│   ├── aircube_replay_script.py   # Replay logged CSV with timing
│   ├── build_exe.py               # PyInstaller build for desktop app
│   ├── build_tray.py              # PyInstaller build for tray app
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
├── README.md              # Customer-facing product page
├── HOME_ASSISTANT.md      # Home Assistant integration guide
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
  ├── Init: NVS, I2C, serial, LED, history, button, ENS210, ENS16X, Zigbee
  ├── xTaskCreate(sensor_task)    -- reads sensors, sends JSON, logs history, pushes Zigbee
  ├── xTaskCreate(command_task)   -- polls for incoming serial commands
  └── Main loop (20ms tick)       -- smooth LED color transitions based on AQI
```

### Data flow

```
ENS210 (I2C)  ──► sensor_task ──► serial JSON output (USB)
ENS16X (I2C)  ──►      │       ├─► history_record_sample() ──► flash ring buffer
                        │       └─► zigbee_update_sensors()  ──► Zigbee attribute reports
                        │
                   AQI value ──► main loop ──► LED color (green-to-red hue mapping)
```

### Module overview

**Sensors**

- `ens210.c` -- I2C driver for the ENS210 temperature/humidity sensor. Exposes `ens210_get_temperature()`, `ens210_get_humidity()`.
- `ens16x_driver.c` -- I2C driver for the ENS16X air quality sensor. Reads eTVOC, eCO2, AQI, and AQI-UBA. Accepts environmental compensation data from the ENS210.

**LED**

- `led.c` -- Thread-safe color and intensity control for WS2812 LEDs. Uses a mutex so any task can call `led_set_color()` / `led_set_intensity()`.
- `led_color_lib.c` -- Converts a 16-bit hue to a 24-bit GRB value via `get_color_from_hue()`.
- `ws2812_control.c` -- Low-level RMT peripheral driver for WS2812 timing.

**Communication**

- `serial_protocol.c` -- JSON-over-USB serial interface. Sends periodic sensor data, accepts commands (see Serial Protocol below).
- `zigbee.c` -- Registers a Zigbee End Device on the ESP32-H2's native 802.15.4 radio. Exposes temperature/humidity via standard ZCL clusters and eCO2/eTVOC/AQI via custom cluster 0xFC01.

**Storage**

- `history.c` -- Append-only ring buffer on a dedicated flash partition. Accumulates sensor samples in RAM and flushes a min/avg/max summary every 5 minutes. Stores up to 7 days (2016 entries). Each slot is exactly 32 bytes.

**Input**

- `button.c` -- GPIO debounce with short press (brightness cycle) and long press (Zigbee pairing).

---

## Serial Protocol Reference

The AirCube communicates over USB-Serial-JTAG at **115200 baud**. All messages are single-line JSON terminated by `\n`.

### Device output (sent every readout period, default 1s)

```json
{
  "ens210": {"status": 0, "temperature_c": 23.45, "temperature_f": 74.21, "humidity": 52.30},
  "ens16x": {"status": "OK", "etvoc": 42, "eco2": 415, "aqi": 3, "aqi_uba": 1},
  "timestamp": 12345
}
```

`timestamp` is milliseconds since boot.

### Commands (send to device)

All commands are JSON with a `"cmd"` field. Send a complete JSON object followed by `\n`.

| Command | Payload | Response |
|---------|---------|----------|
| `get_config` | `{"cmd":"get_config"}` | `{"config":{"intensity":0.60,"readout_period":1000}}` |
| `set_intensity` | `{"cmd":"set_intensity","value":0.3}` | `{"status":"ok","cmd":"set_intensity","value":0.30}` |
| `set_readout_period` | `{"cmd":"set_readout_period","value":500}` | `{"status":"ok","cmd":"set_readout_period","value":500.00}` |
| `get_history_info` | `{"cmd":"get_history_info"}` | `{"history_info":{"entries":288,"capacity":2016,"slot_bytes":32,"window_us":300000000}}` |
| `get_history` | `{"cmd":"get_history","start":0,"count":48}` | `{"history":[...],"start":0,"count":48}` |
| `clear_history` | `{"cmd":"clear_history"}` | `{"status":"ok","cmd":"clear_history","value":0.00}` |

**Shortcut:** Typing just `h` in the serial monitor dumps the entire history as CSV.

### Intensity range

`set_intensity` accepts `0.0` (off) to `1.0` (full brightness).

### Readout period range

`set_readout_period` accepts `100` to `10000` (milliseconds).

### History slot format

Each history entry contains min/avg/max for all five measurements over one 5-minute window. Temperature and humidity are stored as `int16 x 100` (e.g., 2345 = 23.45 C). AQI, eCO2, and eTVOC are raw uint16 values.

Abbreviated JSON keys in `get_history` responses:

| Key | Meaning |
|-----|---------|
| `seq` | Sequence number |
| `t_a`, `t_n`, `t_x` | Temperature avg, min, max (x100 C) |
| `h_a`, `h_n`, `h_x` | Humidity avg, min, max (x100 %) |
| `q_a`, `q_n`, `q_x` | AQI avg, min, max |
| `c_a`, `c_n`, `c_x` | eCO2 avg, min, max (ppm) |
| `v_a`, `v_n`, `v_x` | eTVOC avg, min, max (ppb) |

---

## Zigbee Integration

The ESP32-H2 has a native IEEE 802.15.4 radio. AirCube registers as a Zigbee End Device with the following clusters on **endpoint 10**:

| Cluster | ID | Attributes |
|---------|----|-----------|
| Temperature Measurement | 0x0402 | `measuredValue` (int16, x100 C) |
| Relative Humidity | 0x0405 | `measuredValue` (uint16, x100 %) |
| Custom Air Quality | 0xFC01 | `eco2` (0x0000), `etvoc` (0x0001), `aqi` (0x0002) -- all uint16 |

The custom cluster requires a **ZHA quirk** or **Zigbee2MQTT external converter** on the Home Assistant side. Both are included in the repo (`zha/aircube.py` and `z2m/aircube.js`).

See [HOME_ASSISTANT.md](HOME_ASSISTANT.md) for setup instructions.

### Pairing behavior

- On first boot, the device automatically starts network steering (searching for a coordinator).
- A 3-second button hold triggers pairing mode at any time.
- During pairing, the LED flashes blue at 2 Hz.
- Pairing mode times out after 60 seconds if no network is found.

---

## Desktop Apps (scripts/)

### Prerequisites

```bash
cd scripts
pip install -r requirements.txt
```

### aircube_app.py -- Full desktop GUI

Live sensor display, color-coded AQI, three-panel charts (temp/humidity, AQI, gas levels), optional CSV logging, configurable history depth (50--1000 points).

```bash
python aircube_app.py
```

### aircube_tray.py -- System tray monitor (Windows)

Shows AQI as a number in the Windows taskbar. Color-coded icon, tooltip with all readings, optional AQI threshold alerts, start-with-Windows option.

```bash
python aircube_tray.py
```

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
python build_tray.py    # Produces dist/AirCubeTray.exe
```

---

## Hardware

### Components

| Part | Description |
|------|------------|
| ESP32-H2-MINI-1 | MCU with 802.15.4 (Zigbee/Thread) radio |
| ENS210 | Temperature and humidity sensor (I2C) |
| ENS161 / ENS16X | Air quality sensor -- eTVOC, eCO2, AQI (I2C) |
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
