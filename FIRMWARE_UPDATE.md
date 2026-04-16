# AirCube Firmware Update Guide

This guide walks you through updating your AirCube firmware using Espressif's web-based [ESP Launchpad](https://espressif.github.io/esp-launchpad/). It uses WebUSB, so you can flash directly from your browser -- no desktop tools to install.

## Prerequisites

- **Browser:** Google Chrome or Microsoft Edge (Safari and Firefox do not support WebUSB)
- **OS:** Windows, macOS, or Linux
- **Cable:** A data-capable USB-C cable (charge-only cables won't work)
- **Firmware:** Download the latest `.bin` file from [GitHub Releases](https://github.com/StuckAtPrototype/AirCube/releases)

## Step-by-Step

### 1) Download the firmware

1. Go to the [latest release](https://github.com/StuckAtPrototype/AirCube/releases).
2. Under **Assets**, download the `AirCube_firmware_v*.bin` file for the version you want.
3. Save it somewhere you can find it (e.g. your Downloads folder).

### 2) Open ESP Launchpad

1. Navigate to [https://espressif.github.io/esp-launchpad/](https://espressif.github.io/esp-launchpad/).
2. Keep this tab open throughout the process.

### 3) Connect your AirCube

1. Plug the USB-C cable into your AirCube and your computer.
2. If AirCube is already plugged in, unplug it and plug it back in to make sure it's detected.
3. Close any serial monitors or apps that might be using the port (e.g. AirCube Tray, Arduino Serial Monitor).

### 4) Select DIY mode and add the firmware file

1. In ESP Launchpad, select the **DIY** tab from the top menu.
2. Click **Add File** and select the firmware `.bin` you downloaded.
3. Set the **Flash Address** to `0x0`.
4. Verify the file and address appear in the table.

### 5) Connect to the device

1. Click **Connect** in the top menu.
2. Select your AirCube from the WebUSB device prompt. It may appear as "USB JTAG/serial debug unit" or similar.
3. If you don't see it, check the [Troubleshooting](#troubleshooting) section below.

### 6) Program

1. Click **Program** to start flashing.
2. Watch the Console area for progress. Wait until you see a completion message.

### 7) Reset the device

1. When programming is complete, unplug the USB-C cable and plug it back in.
2. AirCube will boot the new firmware. The LED should light up green after a few seconds.

## Troubleshooting

- **No device in the Connect dialog:** Make sure another app isn't using the serial port. Try a different USB port or cable. On some cables, flipping the USB-C connector helps.
- **Browser not supported:** Use Chrome or Edge. Safari and Firefox do not support WebUSB.
- **Programming fails:** Disconnect and reconnect AirCube, make sure the flash address is `0x0`, and try again.
- **Console output is garbled:** Set the Console Baudrate to `115200` in ESP Launchpad settings.
- **LED doesn't come on after flashing:** Unplug and replug the USB-C cable. If it still doesn't work, try flashing again.
- **Linux:** If the device doesn't appear, you may need to add udev rules for the ESP USB device. Unplug, replug, and try Connect again.
- **Charge-only cable:** Some USB-C cables only carry power. Use a cable that supports data -- the same one that works with the desktop app will work here.

## Success

After resetting, the AirCube LED will light up and begin showing air quality colors within a few seconds. The sensor needs about 3 minutes to warm up before readings stabilize.

Your settings (brightness level, Zigbee pairing) are preserved across firmware updates.
