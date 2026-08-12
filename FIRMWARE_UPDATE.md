# AirCube Firmware Update Guide

Update your AirCube from the browser with [AirCube Web](https://stuckatprototype.github.io/AirCube/). It talks to the cube over USB using the Web Serial API, so there is nothing to install -- and once you are connected you also get live readings, the LED brightness control and your history, the same as the desktop app.

## Prerequisites

- **Browser:** Google Chrome or Microsoft Edge (Safari and Firefox do not support Web Serial)
- **OS:** Windows, macOS, Linux, or ChromeOS
- **Cable:** A data-capable USB-C cable (charge-only cables won't work)

## Step-by-Step

### 1) Open AirCube Web

Go to [stuckatprototype.github.io/AirCube](https://stuckatprototype.github.io/AirCube/).

### 2) Connect your AirCube

1. Plug the USB-C cable into your AirCube and your computer.
2. Close anything else that might be holding the serial port (AirCube Tray, Arduino Serial Monitor, `idf.py monitor`).
3. Click **+ Connect** and pick your AirCube from the browser's device prompt. It appears as "USB JTAG/serial debug unit" or similar.

With one cube connected the app opens straight to its page and starts showing live readings.

### 3) Flash the firmware

1. Open the **⋯** menu on the device page and choose **Flash firmware**. (You can also reach it from Settings → Firmware.)
2. Pick the version you want. The newest release is marked **(latest)**.
3. Leave the flash offset at **0x0 (merged release image)**.
4. Click **Flash** and watch the log. It takes about 30 seconds.

The cube reboots itself when the write finishes and reconnects automatically. The LED should light up green after a few seconds.

Your settings -- brightness level and Zigbee pairing -- are preserved across firmware updates.

## Flashing a development build

If you built the firmware yourself with `idf.py build`, you have two options:

- Flash `build/AirCube.bin` (the app only) at offset **0x10000**.
- Or run `esptool merge-bin` first and flash the merged image at **0x0**.

Use the **Local .bin** button in the flash dialog to pick the file, then choose the matching offset.

## Troubleshooting

- **No device in the connect dialog:** Make sure another app isn't using the serial port. Try a different USB port or cable. On some USB-C cables, flipping the connector helps.
- **Browser not supported:** Use Chrome or Edge on a desktop OS. Safari and Firefox do not implement Web Serial, and iOS browsers cannot access USB devices at all.
- **"Failed to open serial port":** Another tab or application already has it. Close AirCube Tray and any serial monitors, then click Refresh in the dialog.
- **Flashing fails partway:** Unplug the AirCube, plug it back in, and try again. The bootloader is only reached at reset, so a fresh power cycle usually clears it.
- **Charge-only cable:** Some USB-C cables carry power but no data. Use the same cable that works with the desktop app.
- **Linux:** If the device never appears, your user may need permission for the ESP USB device. Add a udev rule for `303a:1001` or add yourself to the `dialout` group, then replug.
- **The cube doesn't reconnect after flashing:** Unplug and replug the USB-C cable, then click Connect again.

## Alternatives

- **[AirCube Tray](https://github.com/StuckAtPrototype/AirCubeTray)** -- the Windows desktop app, which can also flash over USB.
- **[ESP Launchpad](https://espressif.github.io/esp-launchpad/)** -- Espressif's generic web flasher. Use the **DIY** tab, add the `.bin` from [Releases](https://github.com/StuckAtPrototype/AirCube/releases), and set the flash address to `0x0`.
- **esptool** -- `esptool --chip esp32h2 -p PORT write-flash 0x0 AirCube_firmware_vX.Y.Z.bin`

## Success

After it reboots, the AirCube LED lights up and begins showing air quality colors within a few seconds. The gas sensor needs about 3 minutes to warm up before readings stabilize.
