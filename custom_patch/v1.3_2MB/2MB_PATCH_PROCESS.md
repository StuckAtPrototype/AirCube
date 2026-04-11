# AirCube v1.3 Firmware — 2MB Flash Rehosting Patch

> How to make the official v1.3 release flashable on AirCube hardware with a 2 MB flash chip, without recompiling.

## Background

The official `AirCube_firmware_v.1.3.bin` release from StuckAtPrototype is compiled with `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, targeting a 4 MB flash chip. However, some AirCube units — including the v1.1 hardware revision — ship with a **2 MB flash chip** (ESP32-H2 with 2 MB variant). Flashing the unmodified v1.3 binary to a 2 MB chip produces a boot crash:

```
E spi_flash: Detected size(2048k) smaller than the size in the binary image header(4096k). Probe failed.
assert failed: __esp_system_init_fn_init_flash
```

This document describes a binary patch process that fixes the issue **without requiring a firmware rebuild**. The patch modifies four specific things in the release binary and leaves all executable code untouched.

> **This is a temporary workaround, not a permanent fix.** The correct long-term solution is for the upstream source and release process to produce a 2 MB-compatible binary natively — see "Important notes" below. Until that upstream fix lands, this patch process needs to be re-applied to every new firmware release built for 4 MB flash. Once upstream ships a 2 MB variant, this document and its associated tooling should be considered superseded and can be retired.

> **Important:** the application code is byte-identical between the original and patched binary. This patch is metadata-only — only the bootloader's flash-size header, the application's flash-size header, the partition table, and the application's SHA-256 validation hash are modified. This means all behavioral analysis in `BINARY_NOTES.md` applies equally to the original release and to the 2MB-rehosted variant. See section "Why the app code is unchanged" at the end of this document for details.

---

## What the v1.3 firmware binary actually contains

The `AirCube_firmware_v.1.3.bin` release is a **merged flash image** — not just the application. It contains three regions laid out for a 4 MB flash:

| Offset     | Region            | Size      |
|------------|-------------------|-----------|
| `0x000000` | Bootloader        | 32 KB     |
| `0x008000` | Partition table   | 4 KB      |
| `0x010000` | Application (app) | ~613 KB   |

The original partition table inside the binary reserves a **3 MB** factory app partition, which by itself extends well beyond the 2 MB chip boundary. Even if the app boots, the `zb_storage`, `zb_fct`, and `history` partitions that follow the factory partition land at addresses the chip cannot physically address.

---

## What gets changed

Four targeted patches are applied to the merged binary. None of them modify the application's executable code. All can be done in under 50 lines of Python.

### Patch 1 — Bootloader flash-size header byte

The ESP image header stores the flash size in the high nibble of byte offset `0x0003`:

- `0x2` = 4 MB
- `0x1` = 2 MB

The bootloader at offset `0x0000` has byte `0x0003` changed from `0x2f` → `0x1f`:

```python
patched[3] = (patched[3] & 0x0F) | 0x10  # set high nibble to 0x1 (2MB)
```

### Patch 2 — Application flash-size header byte

The application image at offset `0x10000` has its own copy of the same header byte. It's patched identically:

```python
patched[0x10003] = (patched[0x10003] & 0x0F) | 0x10
```

### Patch 3 — Partition table (critical)

The original partition table at `0x8000` defines the factory app partition as 3 MB (`0x300000`), placing the Zigbee storage and history partitions at addresses beyond the 2 MB chip limit. All four data partitions must be relocated to fit within 2 MB:

| Partition   | Original offset | Original size | Patched offset | Patched size      |
|-------------|-----------------|---------------|----------------|-------------------|
| nvs         | `0x009000`      | 24 KB         | `0x009000`     | 24 KB (unchanged) |
| phy_init    | `0x00f000`      | 4 KB          | `0x00f000`     | 4 KB (unchanged)  |
| factory     | `0x010000`      | **3072 KB**   | `0x010000`     | **1848 KB**       |
| zb_storage  | `0x310000`      | 64 KB         | `0x1de000`     | 64 KB             |
| zb_fct      | `0x320000`      | 4 KB          | `0x1ee000`     | 4 KB              |
| history     | `0x321000`      | 68 KB         | `0x1ef000`     | 68 KB             |

The factory app partition shrinks from 3072 KB to 1848 KB. The v1.3 application binary is only about 613 KB, so this leaves over 1.2 MB of headroom for future releases — meaning this patch should continue to work for a while before the app genuinely outgrows the space.

All partitions now end exactly at `0x200000` (2 MB), fully utilizing the chip.

After relocating the entries, a new MD5 checksum must be computed over the updated partition entries and written into the partition table footer. ESP-IDF bootloaders verify this MD5 before using the table.

### Patch 4 — Application SHA-256 hash update

The ESP-IDF bootloader validates the application image using a SHA-256 hash stored in the last 32 bytes of the image (at offset `image_size - 32`). The hash covers the entire image data with **no masking** — patching byte `0x10003` in Patch 2 invalidates the stored hash.

The hash must be recomputed after all other patches and written back:

```python
image_size = 561792  # from `esptool image-info`
new_hash = hashlib.sha256(bytes(app[:image_size - 32])).digest()
app[image_size - 32:image_size] = new_hash
```

Without this step, the bootloader reports:

```
E esp_image: Image hash failed - image is corrupt
E boot: Factory app partition is not bootable
```

---

## How to verify the patch

Use `esptool` to inspect the patched binary:

```bash
python3 -m esptool image-info AirCube_v1.3_2MB_final.bin
```

**Tested with `esptool v5.x`.** The `image-info` command's output format changed between esptool 4.x and 5.x, so earlier versions may display differently.

Expected output includes:

```
Flash size: 2MB
Checksum: valid
Validation hash: <new hash> (valid)
```

If any of those lines show `4MB` or `invalid`, one of the patches wasn't applied correctly.

You can also inspect the partition table separately:

```bash
python3 -m esptool --chip esp32h2 image-info AirCube_v1.3_2MB_final.bin
# or extract and view the partition table directly:
python3 -m esptool read_flash 0x8000 0x1000 partitions.bin
python3 ~/esp/esp-idf/components/partition_table/gen_esp32part.py partitions.bin
```

The last command pretty-prints the partition table. All entries should have offsets + sizes that sum to values ≤ `0x200000`.

---

## How to flash the patched binary

The easiest path is the Espressif web flasher at `https://espressif.github.io/esptool-js/`:

1. Put the AirCube in download mode (hold the button while plugging in USB, or hold the button and press reset)
2. Open the web flasher
3. Click "Connect" and select the AirCube's serial port
4. Add the patched bin at address `0x0`
5. Click "Program"

Alternatively, via command line:

```bash
python3 -m esptool --chip esp32h2 --port /dev/ttyACM0 --baud 460800 \
    write_flash 0x0 AirCube_v1.3_2MB_final.bin
```

---

## Important notes

- **The `zb_storage` partition is not erased by this patch.** If the device was previously paired (including with the pre-patch firmware on a unit that somehow booted), it will attempt to reconnect to the same Zigbee network after flashing.
- **If a clean Zigbee re-pair is desired**, hold the AirCube button for 3 seconds after booting. This calls `esp_zb_factory_reset()` internally, which wipes `zb_storage` and `zb_fct` and enters pairing mode.
- **This patch has to be redone for every firmware release — but only until the upstream build process is fixed to respect the hardware limitations.** The offsets above are stable across builds, but the application size and SHA-256 hash change every time a new release is cut, so the patch script must be re-run against each new `AirCube_firmware_vX.Y.bin` that's built for 4 MB flash. Once the upstream source and release process produce a 2 MB-compatible binary natively (see next bullet), this patch process becomes unnecessary and can be retired.
- **For an upstream fix**, the clean solution would be a separate build target with `CONFIG_ESPTOOLPY_FLASHSIZE_2MB` and a 2 MB-friendly `partitions.csv`, shipped as a second binary in each release (e.g., `AirCube_firmware_vX.Y_2MB.bin` alongside the 4 MB variant). This patch process exists as a temporary workaround because such a build wasn't available at the time of need, and should be considered superseded as soon as the upstream release process accounts for both flash sizes.
- This patch was validated on: **ESP32-H2 revision v1.2, 2 MB flash**, MAC `74:4D:BD:FF:FE:69:08:DB`.

---

## Why the app code is unchanged

The four patches above can be grouped by what they touch:

| Patch | Target      | Byte count | Affects executable code? |
|-------|-------------|------------|--------------------------|
| 1     | Bootloader header, offset `0x0003` | 1 byte  | No — header metadata only |
| 2     | App header, offset `0x10003`       | 1 byte  | No — header metadata only |
| 3     | Partition table at `0x8000`        | ~100 bytes | No — lives outside both bootloader and app |
| 4     | App SHA hash, last 32 bytes of image | 32 bytes | No — validation metadata only |

**None of the application's `.text`, `.rodata`, `.data`, or `.bss` segments are touched.** The exact same RISC-V instructions run on a 2MB-patched unit as on a 4MB-native unit. This means anything you learn about firmware behavior by analyzing the 2MB bin (as documented in `BINARY_NOTES.md`) applies equally to the official 4MB release.

The only behavioral difference between the two builds is that the 2MB variant has less room for future app growth before the factory partition fills up.

---

## Reference implementation

A minimal Python script that performs all four patches:

```python
#!/usr/bin/env python3
"""Patch AirCube v1.3 firmware for 2MB flash."""
import hashlib
import struct
import sys

def patch_2mb(input_path, output_path):
    with open(input_path, 'rb') as f:
        data = bytearray(f.read())

    # Patch 1: bootloader flash-size byte
    data[0x03] = (data[0x03] & 0x0F) | 0x10

    # Patch 2: application flash-size byte
    data[0x10003] = (data[0x10003] & 0x0F) | 0x10

    # Patch 3: partition table — this requires rewriting the 32-byte entries
    # starting at 0x8000. Each entry is:
    #   magic(2) + type(1) + subtype(1) + offset(4) + size(4) + label(16) + flags(4)
    # See components/partition_table/gen_esp32part.py in ESP-IDF for the full format.
    # Relocate factory, zb_storage, zb_fct, history per the table above.
    # Then recompute the MD5 stored in the "0xEBEB..." marker entry.
    # (Implementation elided for brevity — use gen_esp32part.py as a reference.)

    # Patch 4: recompute application SHA-256
    # The app starts at 0x10000. Get its true size from the image header.
    app_start = 0x10000
    # ESP image: magic(1) + seg_count(1) + flash_mode(1) + flash_sf(1) + entry(4)
    # followed by 16-byte extended header, then segments. Walk segments to find end.
    image_size = _compute_app_size(data, app_start)
    hash_offset = app_start + image_size - 32
    new_hash = hashlib.sha256(bytes(data[app_start:hash_offset])).digest()
    data[hash_offset:hash_offset + 32] = new_hash

    with open(output_path, 'wb') as f:
        f.write(data)

def _compute_app_size(data, start):
    """Walk ESP image segments to find total app size."""
    seg_count = data[start + 1]
    off = start + 0x18  # past extended header
    for _ in range(seg_count):
        _, length = struct.unpack('<II', data[off:off + 8])
        off += 8 + length
    # Add checksum padding to 16-byte boundary + 1 checksum byte
    off = ((off + 15) & ~15) + 1
    # Hash appended if SHA append is enabled
    return off - start + 32

if __name__ == '__main__':
    patch_2mb(sys.argv[1], sys.argv[2])
```

**This script is a sketch, not a drop-in tool.** The partition-table rewrite in Patch 3 is non-trivial and is best handled by generating a fresh partition table with ESP-IDF's `gen_esp32part.py` from a modified `partitions.csv`, then splicing the resulting bytes into the merged binary at offset `0x8000`. A full working implementation is left as an exercise for anyone upstreaming this into a proper tool.

---

## Document history

**Original version:** April 2026. Validated on ESP32-H2 rev v1.2, 2 MB flash, AirCube v1.1 hardware. Patch process authored by the AI that originally produced `AirCube_v1_3_2MB_final.bin`; this document consolidates and explains that work in service of making the process reproducible by others.