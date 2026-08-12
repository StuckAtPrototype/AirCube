/* Firmware flashing over Web Serial via esptool-js.
 *
 * Mirrors aircubeapp/flash/flasher.py: chip esp32h2, 460800 baud, merged
 * release images at 0x0 and app-only development builds at 0x10000.
 *
 * The AirCube enumerates as 303A:1001, the ESP32-H2's built-in USB Serial/JTAG
 * interface. esptool-js recognises that product ID and picks its
 * UsbJtagSerialReset strategy on its own, so no custom DTR/RTS sequence is
 * needed here.
 */

import { ESPLoader, Transport } from "../vendor/esptool-js/bundle.js";

export const FLASH_BAUD = 460800;
export const MERGED_IMAGE_ADDRESS = 0x0;
export const APP_ONLY_ADDRESS = 0x10000;
const EXPECTED_CHIP = /ESP32-H2/i;

/**
 * Restart the cube into the firmware that was just written.
 *
 * esptool-js's own "hard_reset" only lowers RTS, which assumes an external
 * USB-UART bridge still holding EN asserted from the connect sequence. The
 * AirCube talks over the ESP32-H2's built-in USB Serial/JTAG, where EN has to
 * be strobed instead. DTR stays low so IO0 is released and the chip boots the
 * application rather than the bootloader again; without this the cube sits in
 * download mode until it is physically unplugged.
 *
 * @param {import("../vendor/esptool-js/bundle.js").Transport} transport
 */
async function rebootIntoApp(transport) {
  await transport.setDTR(false);
  await transport.setRTS(true);
  await new Promise((resolve) => setTimeout(resolve, 150));
  await transport.setRTS(false);
}

/**
 * @param {object} options
 * @param {import("./devices.js").Device} options.device
 * @param {Uint8Array} options.image
 * @param {number} options.address
 * @param {(line: string, kind?: "info"|"err"|"ok") => void} options.onLog
 * @param {(fraction: number) => void} options.onProgress
 * @param {boolean} [options.ignoreChipMismatch]
 */
export async function flashDevice({
  device,
  image,
  address,
  onLog,
  onProgress,
  ignoreChipMismatch = false,
}) {
  const log = (line, kind) => onLog?.(line, kind);
  const terminal = {
    clean() {},
    writeLine(data) {
      log(data);
    },
    write(data) {
      if (data.trim()) log(data);
    },
  };

  log("Closing the data connection so the flasher can take the port...");
  await device.holdForFlash();

  const transport = new Transport(device.port, false);
  const loader = new ESPLoader({ transport, baudrate: FLASH_BAUD, terminal });

  try {
    log("Connecting to the bootloader...");
    const chip = await loader.main();
    log(`Detected ${chip}`, "ok");

    if (!EXPECTED_CHIP.test(chip) && !ignoreChipMismatch) {
      throw new Error(
        `Expected an ESP32-H2 but this device reports "${chip}". Refusing to flash.`,
      );
    }

    log(`Writing ${image.length} bytes at 0x${address.toString(16)}...`);
    await loader.writeFlash({
      fileArray: [{ data: image, address }],
      // "keep" preserves the flash mode, frequency and size already encoded in
      // the image header (DIO / 48MHz / 2MB for AirCube) instead of rewriting it.
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex, written, total) => {
        if (total > 0) onProgress?.(written / total);
      },
    });

    log("Resetting the device...");
    await rebootIntoApp(transport);
    log("Done. The AirCube is rebooting into the new firmware.", "ok");
  } finally {
    try {
      await transport.disconnect();
    } catch {
      // Expected when the USB Serial/JTAG interface re-enumerates on reset.
    }
    await device.releaseFlashHold();
  }
}

/**
 * After a hard reset the USB Serial/JTAG interface re-enumerates, so the port
 * is briefly unusable. Retry the reconnect for a few seconds before giving up.
 */
export async function reconnectAfterFlash(device, { attempts = 10, delayMs = 700 } = {}) {
  try {
    for (let i = 0; i < attempts; i++) {
      await new Promise((resolve) => setTimeout(resolve, delayMs));
      try {
        await device.connect();
        return true;
      } catch {
        /* still enumerating */
      }
    }
    return false;
  } finally {
    device.endFlashGrace();
  }
}
