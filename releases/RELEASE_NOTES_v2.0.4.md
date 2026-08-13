# AirCube firmware v2.0.4

This release keeps the LED display stable through brief Zigbee radio recovery.

## What changed

- Removed the green LED blink when Zigbee reconnects after a parent link failure.
- Preserved the current air-quality color, brightness, hue, and auto-dim state
  across warm MCU restarts.
- Resumed the Pro model's running SCD41 measurement cycle after a warm restart
  instead of restarting the sensor.
- Kept the existing blue pairing-mode indication unchanged.

## Flashing

`AirCube_firmware_v2.0.4.bin` is a merged ESP32-H2 image. Flash it at offset
`0x0`.
