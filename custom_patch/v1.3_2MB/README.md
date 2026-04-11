# AirCube Binary Notes

> Reverse-engineering observations from the v1.3 firmware.

## Important caveats — please read first

**This document is based on analysis of the v1.3 firmware as shipped by StuckAtPrototype.** The specific file analyzed (`AirCube_v1_3_2MB_final.bin`) was a 2MB-rehosted variant of the official `AirCube_firmware_v.1.3.bin` release. **The application code is byte-identical to the official release** — the 2MB variant modifies only four things: the bootloader's flash-size header byte, the application's flash-size header byte, the partition table at offset `0x8000`, and the SHA-256 validation hash in the last 32 bytes of the image. None of those patches modify executable code. All code-behavior claims in this document therefore apply to both the original 4MB release and the 2MB-rehosted build. See the companion document `2MB_PATCH_PROCESS.md` for the details of the non-code modifications.

Other important caveats:

- The analyzed build self-reports as `Project name: AirCube`, `App version: Windows_App_v1.0-13-g20e5139`, compiled `Mar 3 2026 19:04:29`, ESP-IDF v5.5.1.
- The matching `zigbee.c` source for this build did **not** appear in the public `dev` branch of this repo at the time of analysis — only the serial/USB-CDC variant lived there. The Zigbee firmware was presumably built from a different branch that couldn't be located publicly.
- This is binary archaeology. It is useful as a starting point and as an independent sanity check, **not** as authoritative source documentation. Confidence levels are noted throughout. Any disagreement between this document and the actual source should be resolved in favor of the source, and this document should be corrected.

**Every claim in this document should still be vetted against actual source before being acted on.** The analysis was done by:

1. Parsing the merged ESP32-H2 image with `esptool image_info`
2. Disassembling the IROM segment with Capstone (RISC-V 32-bit + compressed extension)
3. Tracing call patterns, string cross-references, and ESP-Zigbee SDK function signatures

This is binary archaeology — useful as a starting point and as an independent sanity check, **not** as authoritative source documentation. Confidence levels are noted throughout. Any disagreement between this document and the actual source should be resolved in favor of the source, and this document should be corrected.

The analysis was prompted by a real-world bug where three sensors (eCO2, eTVOC, AQI) reported `Unknown` in Home Assistant after pairing. The fix turned out to be on the Python quirk side, not the firmware — but the diagnostic path required understanding what the firmware was actually doing on the wire. Section 6 ("Quirk-side bug and fix") is the most important practical takeaway.

---

## 1. Image layout

**Confidence: HIGH** — directly read from `esptool image_info` output and verified with manual offset checks.

The analyzed bin is a merged flash image, not a standalone app:

| Offset    | Contents                                |
|-----------|-----------------------------------------|
| `0x00000` | Bootloader (32 KB partition; ~9 KB of actual loaded segments) |
| `0x08000` | Partition table                         |
| `0x10000` | Factory app partition (the actual firmware) |

Note that the bootloader *partition* is 32 KB but the bootloader's actual loaded code and data sums to only around 9 KB across three segments. The rest is padding. The 2MB patch documentation in `2MB_PATCH_PROCESS.md` has more on the partition layout and how it was shrunk to fit 2MB chips.

**Factory app (extracted):**
- File size: 561,792 bytes (0x89280)
- Project name: `AirCube`
- App version: `Windows_App_v1.0-13-g20e5139`
- Compile time: `Mar 3 2026 19:04:29`
- ESP-IDF version: `v5.5.1`
- Chip target: ESP32-H2
- Image checksum: valid
- Validation hash: valid

**Application segment map:**

| Seg | Load address  | Length    | Type       | Purpose                  |
|-----|---------------|-----------|------------|--------------------------|
| 0   | `0x42070020`  | `0x129b4` | DROM/IROM  | Read-only data (.rodata) |
| 1   | `0x40800000`  | `0x0d63c` | DRAM/IRAM  | Initialized data         |
| 2   | `0x42000020`  | `0x6464c` | DROM/IROM  | Code (.text)             |
| 3   | `0x4080d63c`  | `0x02b48` | DRAM/IRAM  | Data continuation        |
| 4   | `0x40810190`  | `0x0208c` | DRAM/IRAM  | Data continuation        |

The main code segment (segment 2) covers `0x42000020 .. 0x4206466c` — about 410 KB of compiled RISC-V instructions. All function addresses referenced below are in this range.

The version tag `Windows_App_v1.0-13-g20e5139` is `git describe`-style output, suggesting the source for this exact build lives on a branch named `Windows_App` or similar, 13 commits past a `Windows_App_v1.0` tag.

---

## 2. The Zigbee init region

**Confidence: HIGH for the structural claims, MEDIUM for the function-name guesses.**

By searching for cross-references to the string `./main/zigbee.c\0` (which appears at load address `0x42074d38` in segment 0 rodata), the Zigbee init code can be located in segment 2. There are 12 such xrefs, all of them load-immediate sequences feeding the standard ESP-IDF `ESP_ERROR_CHECK` macro expansion. The xrefs cluster in the address range `0x420106a8 .. 0x42010f28`, which is the body of what is presumably `aircube_zb_init()` (or whatever the equivalent function is named in the source).

The error-check string artifacts inside this region show the following ESP-Zigbee SDK calls being made at init time:

```c
esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUFACTURER_NAME)
esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_IDENTIFIER)
esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE)
esp_zb_cluster_list_add_identify_cluster(cluster_list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE)
esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, esp_zb_temperature_meas_cluster_create(&temp_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE)
esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, esp_zb_humidity_meas_cluster_create(&hum_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE)
esp_zb_custom_cluster_add_custom_attr(custom_cluster, ATTR_ECO2_ID,  ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &default_val)
esp_zb_custom_cluster_add_custom_attr(custom_cluster, ATTR_ETVOC_ID, ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &default_val)
esp_zb_custom_cluster_add_custom_attr(custom_cluster, ATTR_AQI_ID,   ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &default_val)
esp_zb_cluster_list_add_custom_cluster(cluster_list, custom_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE)
esp_zb_start(false)
esp_zb_platform_config(&config)
```

**The critical observation:** the custom-cluster attribute access flags are `READ_ONLY | REPORTING` — **without** `ESP_ZB_ZCL_ATTR_MANUF_SPEC`. This is what causes the SDK to encode reports on cluster `0xFC01` as standard (non-manufacturer-specific) ZCL frames despite the cluster ID being in the manufacturer-specific range. See sections 4 and 6 below for why this matters.

The endpoint declared by the firmware is **endpoint 10**, with profile `0x0104` (HA) and device type `0x0302` (Temperature Sensor — the firmware author chose to declare the device as a basic temperature sensor and tack the custom cluster onto the same endpoint).

---

## 3. The sensor task

**Confidence: HIGH that the function exists at the named address. MEDIUM on its full structure since it wasn't disassembled exhaustively.**

A task named `sensor_task` is created via `xTaskCreate` in `app_main` (or equivalent). The task code itself begins at `0x4200ccde`. Verified by:

- The string `sensor_task\0` appears at load address `0x420737e8` in rodata
- A `lui/addi` pair at `0x4200d0d4 / 0x4200d0d8` loads that string into `a1` (the `pcName` argument to `xTaskCreate`)
- The preceding `lui/addi` at `0x4200d0dc / 0x4200d0e0` loads `0x4200ccde` into `a0` (the `pvTaskCode` function pointer)

The task body is roughly 900 instructions (~0x4200ccde to ~0x4200d824). It saves all callee-preserved registers `s0..s11` in its prologue, indicating a complex loop with many local state variables maintained across iterations.

**What it does each iteration** (high-level pattern, not fully traced):

1. Reads ENS210 (temperature + humidity) and ENS16x (eCO2 + eTVOC + AQI)
2. Logs the readings to the serial console as JSON (the same JSON format the standalone serial firmware emits — `{"ens210":{...},"ens16x":{...},"timestamp":...}`)
3. Calls a serial-protocol function (around `0x4200fcfc` / `0x4200feb8`) for the JSON dump
4. Calls the Zigbee publish function at `0x42010f34` with the five sensor values
5. Sleeps and repeats

**The argument-shuffling for the Zigbee publish call is interesting and worth verifying against source.** Right before the call, registers are reorganized:

```
a0 = s1   ; temperature
a1 = s2   ; humidity
a2 = s3   ; etvoc    <-- not eco2
a3 = s6   ; aqi
a4 = s0   ; eco2
jal aircube_zb_publish
```

This implies the publish function's signature is `(temperature, humidity, etvoc, aqi, eco2)` — note that `etvoc` and `eco2` are *not* in their natural order. This may simply be how the source was written; worth confirming.

**Confidence MEDIUM on the exact argument-to-sensor mapping** — register-to-source-variable mapping is inferred from how the values are subsequently used inside `aircube_zb_publish`, not directly observed.

---

## 4. The Zigbee publish function

**Confidence: HIGH for the call sequence and constants. The function semantics are unambiguous from the disassembly.**

The function at `0x42010f34` (call it `aircube_zb_publish` for naming purposes) takes 5 u16 sensor values and pushes them into the ESP-Zigbee SDK. Its observed structure:

1. **Save callee-preserved registers** s0..s3 (s0 = arg4, s1 = arg3, s2 = arg2, s3 = arg1)
2. **Gate on a "zb_joined" flag** at byte `0x40816665`. If zero, return immediately without doing anything. This is what causes the function to be a no-op until the device successfully joins the Zigbee network.
3. **Scale temperature and humidity** through helper functions (calls at `0x42010f60` and `0x42010f6a`). The ZCL `MeasuredValue` for `TemperatureMeasurement` is in 0.01°C units, and for `RelativeHumidity` is in 0.01% units, so these are presumably the unit conversions.
4. **Stack the five values** at `sp+0xe` (temp), `sp+0xc` (hum), `sp+0xa` (eco2), `sp+0x8` (etvoc), `sp+0x6` (aqi). These are the value pointers passed to `set_attribute_val`.
5. **Call a "lock zigbee stack" function** at `0x42011e1c` (probably `esp_zb_lock_acquire(portMAX_DELAY)` or similar)
6. **Make five `set_attribute_val` calls** in sequence (see table below)
7. **Make three manual `report_attr_cmd_req` calls** for the custom-cluster attributes — necessary because ZHA doesn't auto-configure reporting on manufacturer-specific clusters, so the firmware has to push reports itself
8. **Release the zigbee stack lock**
9. **Return**

**The five `set_attribute_val` calls** all go through a tiny wrapper at `0x420114c8` (which is a 6-instruction thunk that tail-calls `esp_zb_zcl_set_attribute_val` proper). The wrapper takes the standard 6-argument signature: `(endpoint, cluster_id, role, attr_id, value_ptr, check)`.

Argument values for each call, read directly from the disassembly:

| #  | Sensor | endpoint | cluster_id | role | attr_id | value_ptr   | check |
|----|--------|----------|------------|------|---------|-------------|-------|
| 1  | Temp   | `0x0a`   | `0x0402`   | `1`  | `0x00`  | `sp+0x0e`   | `0`   |
| 2  | Hum    | `0x0a`   | `0x0405`   | `1`  | `0x00`  | `sp+0x0c`   | `0`   |
| 3  | eCO2   | `0x0a`   | `0xFC01`   | `1`  | `0x00`  | `sp+0x0a`   | `0`   |
| 4  | eTVOC  | `0x0a`   | `0xFC01`   | `1`  | `0x01`  | `sp+0x08`   | `0`   |
| 5  | AQI    | `0x0a`   | `0xFC01`   | `1`  | `0x02`  | `sp+0x06`   | `0`   |

Cluster `0xFC01` is encoded in the disassembly as `c.lui a1, 0x10; addi a1, a1, -0x3ff` → `0x10000 + (-0x3ff) = 0xFC01`. The endpoint constant `0x0a` matches the device signature shown in ZHA (endpoint 10).

**Three `report_attr_cmd_req` calls** follow at addresses `0x42010fe2`, `0x42010fe8`, `0x42010fee`, each with `a0 = 0`, `1`, `2` respectively (the three custom-cluster attribute IDs). They go to a wrapper at `0x42010ab6`.

---

## 5. The report-attribute wrapper

**Confidence: HIGH on the struct field values. MEDIUM on the field-to-name mapping** (matched against the public ESP-Zigbee SDK header for `esp_zb_zcl_report_attr_cmd_t`, but the SDK has been revised over time and field offsets may have shifted).

The wrapper function at `0x42010ab6` builds an `esp_zb_zcl_report_attr_cmd_t` struct on the stack and passes it to `esp_zb_zcl_report_attr_cmd_req`. The struct construction observed in the disassembly:

```
sp+0x08..0x1c: zero-initialized (28 bytes)
sp+0x10 = 1                ; zcl_basic_cmd.src_endpoint = 1
sp+0x11 = 0x0a             ; zcl_basic_cmd.dst_endpoint = 10
sp+0x14 = 2                ; address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT
sp+0x18 = 0xFC01           ; cluster_id (loaded as -0x3ff sign-extended)
sp+0x1a |= 0x04            ; flag bit (likely "direction" or similar)
sp+0x1a |= 0x08            ; another flag bit  → final value 0x0C
sp+0x1c = 0xFFFF           ; manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC
sp+0x1e = a0               ; attributeID (passed in by caller: 0, 1, or 2)
```

**The critical detail is `manuf_code = 0xFFFF`.** In the ESP-Zigbee SDK, `0xFFFF` is the sentinel meaning "this is **not** a manufacturer-specific report — send it as a standard ZCL frame." Combined with the cluster being registered in section 2 without the `MANUF_SPEC` access bit, this means: **on the wire, the AirCube sends attribute reports for cluster `0xFC01` as ordinary ZCL frames** that happen to carry a cluster ID in the manufacturer-specific range.

This is a perfectly valid choice — the ZCL spec allows it — but it has a non-obvious consequence on the receiving (ZHA) side. See section 6.

---

## 6. The quirk-side bug and the fix

**Confidence: HIGH. The fix has been verified working on real hardware.**

The original `aircube.py` quirk in this repo declares the three custom-cluster attributes as manufacturer-specific:

```python
eco2 = ZCLAttributeDef(id=0x0000, type=t.uint16_t, is_manufacturer_specific=True)
```

When `is_manufacturer_specific=True`, zigpy expects incoming reports for that attribute to arrive as **manufacturer-specific** ZCL frames — i.e., with the manufacturer-specific bit set in the frame control byte and a manufacturer code in the ZCL header. But as section 5 shows, the firmware sends these reports as **standard** frames (`manuf_code = 0xFFFF`).

The mismatch means zigpy's frame parser routes the incoming attribute values somewhere other than the slot the v2 quirk's `_update_attribute` hook is watching, and HA never sees the values. The attributes stay at their default value forever, which reads as `None` via "Read attribute" in the ZHA Manage Zigbee Device dialog.

Temperature and humidity work because they're on standard clusters (`0x0402` and `0x0405`) and never go through the manufacturer-specific code path.

**The fix is one line per attribute** — flip `is_manufacturer_specific` to `False`:

```python
class AttributeDefs(CustomCluster.AttributeDefs):
    eco2  = ZCLAttributeDef(id=0x0000, type=t.uint16_t, is_manufacturer_specific=False)
    etvoc = ZCLAttributeDef(id=0x0001, type=t.uint16_t, is_manufacturer_specific=False)
    aqi   = ZCLAttributeDef(id=0x0002, type=t.uint16_t, is_manufacturer_specific=False)
```

It's also useful to add an explicit `_update_attribute` override to forward updates to the v2 sensor bindings:

```python
def _update_attribute(self, attrid, value):
    super()._update_attribute(attrid, value)
    self.listener_event("attribute_updated", attrid, value)
```

After this change and an HA restart, all five sensors populate normally and react to live changes. **No firmware reflash is needed** — the firmware was correct all along; the bug was purely on the Python side.

---

## 7. Open questions and gaps

Things noticed during analysis but not fully traced. Worth investigating with the actual source:

- **`0x40816664` flag.** Adjacent to the `0x665` "zb_joined" gate. Set in five places, cleared in one, but its purpose was never confirmed. Possibly an "is connecting" or "report pending" flag.
- **The `Windows_App` branch.** The version tag in the bin (`Windows_App_v1.0-13-g20e5139`) implies a branch in the maintainer's git history that wasn't found on the public repo at analysis time.
- **AQI scale.** The firmware sends a value that appears to be the ENS161 **AQI-S** (0–500 range), not the AQI-UBA (1–5 range). Empirically, blowing on the sensor pushes the reported value into the 100–200 range. The serial JSON output has both fields (`aqi` and `aqi_uba`) but only the first is sent over Zigbee. Worth confirming which field the firmware is actually publishing — there may be a case for sending AQI-UBA instead, or both via separate attributes.
- **Sensor read scheduling.** `sensor_task` was not exhaustively disassembled. The exact timing, the FreeRTOS sync mechanisms used, and any error handling for failed sensor reads were not characterized.
- **The `temp_cfg` and `hum_cfg` configs** passed to the standard cluster create functions. They're loaded from rodata at known addresses but the exact field initialization wasn't decoded.
- **Pairing flow.** A string ` starting Zigbee pairing` exists at `0x42074577` and is referenced from code, suggesting a button-triggered pairing function, but the trigger path was not traced.
- **The "0x42011028 error path"** — a separate function at around `0x42010ff8` logs an error, sets the `0x664` flag, clears the `0x665` gate, and appears to reboot or reset. Triggered by what? Worth understanding because if it fires unexpectedly it would silently disable Zigbee publishing.
- **Where the AQI-S vs AQI-UBA selection happens** in the code path between ENS16x driver output and the Zigbee publish call.

---

## 8. Tools and methodology

For anyone wanting to reproduce or extend this analysis:

**Tools used:**

- `esptool image_info` (from the `esptool` Python package) — for parsing the merged-bin layout and segment information
- Python with the `capstone` package (`pip install capstone`) configured for `CS_ARCH_RISCV + CS_MODE_RISCV32 + CS_MODE_RISCVC` — for disassembly. The compressed extension is essential because RV32C is heavily used by ESP-IDF.
- `strings -n N` for finding string artifacts
- Manual offset arithmetic for converting between file offsets, segment-relative offsets, and load addresses

**Useful anchor strings** for navigating the binary:

- `./main/zigbee.c\0` at load addr `0x42074d38` — anchors the Zigbee init region via its xrefs
- `./main/main.c\0` at load addr `0x42073698`
- `sensor_task\0` at load addr `0x420737e8` — used as the task name in `xTaskCreate`
- The `esp_zb_*` strings inside `ESP_ERROR_CHECK` macro expansions — they leak the exact SDK function call sites at init

**The trick that unlocked the analysis:** the ESP-IDF `ESP_ERROR_CHECK` macro stringifies its argument expression and stores it in rodata for the error logger. By searching the binary for unique substrings of those expressions (like `esp_zb_custom_cluster_add_custom_attr`), the init code can be located even when there are no debug symbols. From that anchor, ordinary call-graph analysis takes over.

**The other trick:** to find the `esp_zb_zcl_set_attribute_val` call sites, look for instructions that load specific cluster IDs (`0x0402`, `0x0405`, `0xFC01`) into argument registers near each other. The function-pointer wrappers around the SDK calls aren't visible in any string table, but their argument-register setup patterns are highly distinctive.

**Capstone's quirk to be aware of:** for `jal` instructions, capstone gives the absolute target. For compressed `c.j` and `c.jal`, it gives a PC-relative offset. Mixing the two produces incorrect cross-references; both paths must be handled separately.

---

## Document maintenance

If you correct or extend this document, please:

1. Note which claims you've verified against the canonical source vs which you couldn't verify
2. Add the source file/function names corresponding to the binary addresses listed here, so future readers have a bidirectional mapping
3. If you find that any claim here is wrong, **don't just delete it** — strike it through and explain why, so future readers can learn from the correction
4. Date and sign your edits in this section

**Original version:** Generated April 2026 from analysis of `AirCube_v1_3_2MB_final.bin` — a 2MB-rehosted variant of the official `AirCube_firmware_v.1.3.bin` release. The application code in both builds is byte-identical; only the bootloader flash-size header, partition table, and image hash differ. All addresses, constants, and behavioral descriptions in this document therefore apply to the v1.3 release as shipped, and may differ from other releases.