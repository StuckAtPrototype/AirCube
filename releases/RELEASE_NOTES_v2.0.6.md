# AirCube firmware v2.0.6

This release makes a Pro whose CO2 channel has died say so, instead of reporting
"CO2 UNAVAILABLE" indefinitely with nothing to diagnose.

## What changed

- Counted CO2 failures separately from read failures. A frame that carries good
  temperature and humidity but a rejected CO2 word used to reset the shared
  failure counter, so a sensor that had lost only its CO2 channel never reached
  the recovery path and never reported a fault.
- After five such frames in a row (about 2.5 minutes at the low-power cadence),
  the cube now runs the SCD41's built-in self test once and logs the result,
  then falls back to the existing stop/reinit recovery. The self test is the
  only on-device check that separates an internal sensor malfunction from a
  supply that cannot hold up under the sensor's measurement peaks.
- Added `co2_fails` and `self_test` to `get_sensor_health`, so a unit in the
  field can be triaged over serial without reproducing the fault first.
- Recovered a sensor that has stopped measuring entirely. A stopped SCD41 still
  answers the bus and still reports "not ready", so no failure counter moved and
  the cube reported nothing at all, indefinitely, without logging an error. Three
  missed frames now trigger the same stop/reinit recovery. This state was
  observed once after a firmware flash, where the warm-resume path attached to a
  sensor that was no longer running.

Nothing changes on a healthy unit: the new path only runs after repeated frames
that carry valid temperature and humidity but no CO2 measurement, which a
working sensor does not produce.

## Flashing

The asset is a merged image — bootloader, partition table, and application in
one file, written at offset `0x0`.

| | |
|---|---|
| File | `AirCube_firmware_v2.0.6.bin` |
| Target | ESP32-H2, 2 MB flash |

```
esptool --chip esp32h2 write-flash 0x0 AirCube_firmware_v2.0.6.bin
```

Or in AirCube Web: Flash → Local .bin, offset **0x0**.
