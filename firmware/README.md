# AirCube Firmware

ESP-IDF firmware for the AirCube air quality monitor. Target chip: **ESP32-H2**.

For product documentation, integration guides, and the full architecture/build reference, see:

- [`../README.md`](../README.md) — product overview, sensor readings, LED behavior
- [`../CONTRIBUTING.md`](../CONTRIBUTING.md) — firmware architecture, module layout, serial protocol reference
- [`../FIRMWARE_UPDATE.md`](../FIRMWARE_UPDATE.md) — flashing a prebuilt binary over USB via ESP Launchpad
- [`../docs/BLE_GATT_PROTOCOL.md`](../docs/BLE_GATT_PROTOCOL.md) — BLE GATT protocol reference

## Build and flash

```
idf.py set-target esp32h2
idf.py build
idf.py -p PORT flash monitor
```

See [`../CONTRIBUTING.md`](../CONTRIBUTING.md) for prerequisites and a table of common build issues.

## AirCube Zigbee TX Power

AirCube exposes a build-time Zigbee TX power setting:

- `AirCube Configuration` -> `Zigbee TX power (dBm)` (`CONFIG_AIRCUBE_ZB_TX_POWER_DBM`)
- Default: `20 dBm` (the ESP32-H2's maximum output, for best link margin)
- Supported menuconfig range: `-24` to `20 dBm`

Set it with:

```
idf.py menuconfig
```

Then build and flash as usual. At boot, the firmware logs requested and applied TX power.

Lower values reduce range but can help in dense multi-device environments. Actual applied
power may be limited by hardware, SDK behavior, and regional regulatory limits — the firmware
logs a warning if the applied value doesn't match the requested one.

## Troubleshooting

Program upload failures and other build issues are covered in [`../CONTRIBUTING.md`](../CONTRIBUTING.md)'s
"Common build issues" table. For general ESP-IDF issues, run `idf.py -p PORT monitor` and reboot
the board to check for output logs, or lower the flashing baud rate in `idf.py menuconfig`.

For AirCube-specific bugs or feature requests, open a [GitHub issue](../CONTRIBUTING.md) on this repo.
