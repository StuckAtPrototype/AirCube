# AirCube OTA Firmware Upgrade — Design Proposal

**Status:** proposal, not implemented
**Scope:** over-the-air firmware updates on existing 2 MB hardware, via Zigbee (hub-driven) and BLE (app-driven)
**Target releases:** v2.1.0 (OTA enablement, USB-flashed), v2.2.0 (first OTA-delivered update)

This document proposes a way to add OTA firmware upgrade to AirCube and AirCube Pro.
All size figures are measured against `master` (f9ec414) built with ESP-IDF v5.5.1 for
esp32h2 — see the appendix for how to reproduce them.

---

## 1. Summary

AirCube runs exactly one radio stack per boot: BLE when uncommissioned, Zigbee once
joined to a hub. Covering the whole fleet therefore takes two OTA transports over one
shared flash engine:

- **Zigbee OTA Upgrade cluster (0x0019)** for commissioned devices. Zigbee2MQTT, ZHA,
  and SmartThings all speak it natively, so the hub does the delivery.
- **BLE GATT OTA** for uncommissioned devices, driven by the companion app. Also the
  faster of the two by a wide margin.

Both are modest amounts of firmware work. The blocker is flash: the
ESP32-H2-MINI-1-**H2** module has **2 MB**, the current partition table has a single
1.7 MB `factory` app slot, and OTA needs a second app slot of equal size.

**That blocker has been measured and cleared.**

| Build | App binary | vs. baseline |
|---|---|---|
| **Baseline (as-is)** | **915,184 B** | — |
| **Recommended size config** (§4) | **788,160 B** | **−127,024 (−13.9 %)** |
| + signed images (§7.8) | 823,296 B | −91,888 (−10.0 %) |
| Aggressive reserve (§4.2) | 721,216 B | −193,968 (−21.2 %) |

All figures are the **app partition payload** (`build/AirCube.bin`). Don't compare them
to the GitHub release assets, which are 0x0-based merged images containing bootloader,
partition table, and app: the v2.0.1 asset is 945,200 B, of which the app is ~879,664
(the app sits at offset 0x10000). Master is ~35 KB larger than v2.0.1.

Four flags (five `sdkconfig.defaults` lines), no source changes, bring the image inside the
proposed **832 KB** slot with **63,808 bytes (7.5 %) of headroom**, or 28,672 bytes if
signed images are adopted. The current firmware is built at `-Og` with full logging,
`ESP_ERR_TO_NAME_LOOKUP=y`, and without nano printf, so this is ordinary release tuning.
It is not free of consequences, though — see §4.4.

**Proposed path:** apply the §4 build flags, adopt the §3 partition layout (which keeps
`zb_storage` / `zb_fct` / `history` at their current offsets, so no re-pairing and no
history loss), ship v2.1.0 as a **one-time USB reflash**, then deliver everything after
that over the air.

**One decision needs the maintainer's call before any code is written:** BLE is
currently unauthenticated (`CONFIG_BT_NIMBLE_SECURITY_ENABLE` is off), so a BLE OTA
endpoint lets anyone in radio range push firmware. §7.8 proposes signed images and
outlines the alternatives. It has to be settled first because the public key is
bootstrapped from the running app's own signature block — so it must be present in the
v2.1.0 USB flash, or enabling it later costs a *second* mandatory USB flash.

---

## 2. Current state

### 2.1 Flash layout (`firmware/partitions_zigbee.csv`)

| Name | Type | SubType | Offset | Size | End |
|---|---|---|---|---|---|
| *(bootloader + partition table)* | | | 0x0 | 0x9000 | 0x9000 |
| `nvs` | data | nvs | 0x9000 | 0x6000 | 0xF000 |
| `phy_init` | data | phy | 0xF000 | 0x1000 | 0x10000 |
| `factory` | app | factory | 0x10000 | 0x1A0000 | 0x1B0000 |
| `zb_storage` | data | fat | 0x1B0000 | 0x10000 | 0x1C0000 |
| `zb_fct` | data | fat | 0x1C0000 | 0x1000 | 0x1C1000 |
| `history` | data | 0x99 | 0x1C1000 | 0x11000 | 0x1D2000 |
| *(unallocated)* | | | 0x1D2000 | 0x2E000 | 0x200000 |

There is no `otadata` and no `ota_0`/`ota_1`. `esp_ota_get_next_update_partition()`
returns `NULL` today. 184 KB (0x2E000) at the top of flash is unused.

### 2.2 Relevant firmware facts

- **One image, two hardware models.** `device_model.c` probes for SCD41/VCNL4040 at
  boot and switches behaviour at runtime. Base and Pro share a single binary — worth
  preserving, and it means one `.ota` file covers the whole fleet.
- **Radio modes are exclusive.** `radio_mode.c` picks BLE or Zigbee at boot from NVS
  flags (`zb_joined`, `zb_pair_req`) and switches by `esp_restart()`. This is why two
  transports are needed, and also why they never contend: only one is reachable at a
  time.
- **Firmware version** comes from `esp_app_get_description()->version`, sourced from
  `firmware/version.txt` (currently `2.0.1`). Already surfaced in the Zigbee Basic
  cluster `SW_BUILD_ID` (`zigbee.c:121`) and the BLE Device Info characteristic.
- **The app already drives raw flash** in `history.c` (`esp_partition_read/write/
  erase_range` on the custom `0x99` partition), so partition plumbing is familiar
  ground.
- **`ble_gatt.c` already has the right shape for a bulk transfer.** The history
  streaming path — a dedicated task fed by a queue, `notify_with_retry()` for mbuf
  backpressure, a device-wide single-stream lock shared with USB serial — is close to
  what OTA needs, in the opposite direction. §7 reuses it.
- **Zigbee endpoint is 10**, device ID `ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID`,
  manufacturer name `StuckAtPrototype`, reports use
  `ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC` — i.e. there is **no CSA-assigned
  manufacturer code** in use yet. OTA needs one (§6.3).
- **`esp-zigbee-lib` is pinned to 1.6.7 / `esp-zboss-lib` 1.6.4** because 1.6.8 hangs in
  PHY init on H2. That pin is *fortunate* for OTA: the two worst OTA regressions
  (payload mis-packing, missing block retry) are 2.0.x-only. Do not unpin.

### 2.3 Where the bytes are

`idf.py size-components` on the baseline (flash code 816,656 B; DIRAM 119,650 B of
258,000):

| Archive | Flash code | Note |
|---|---|---|
| `libble_app.a` | 209,234 | BLE controller blob |
| `libzboss_stack.ed.a` | 187,683 | Zigbee stack |
| `libc.a` | 77,207 | newlib — full printf |
| `libbt.a` | 55,114 | NimBLE host |
| `libmain.a` | 35,158 | **all AirCube application code** |
| `libesp_hw_support.a` | 24,086 | |
| `libnvs_flash.a` | 16,641 | |
| `libphy.a` | 13,988 | |
| `libesp_zb_api.ed.a` | 13,548 | |
| `libbtbb.a` | 6,076 | |

AirCube's own code is 35 KB of flash — under 5 % of the image. The rest is vendor
radio stacks, libc, and the IDF drivers/FreeRTOS/esp_system that the residual rows
cover. Shrinking application *code* would be pointless; the wins are all in build
configuration, which is what §4 does.

Release history for context, app-only (release asset minus the 0x10000 offset):
v1.5.2 ≈ 553 KB, v2.0.1 ≈ 859 KB — roughly 55 % growth across one major cycle. Whatever
layout is chosen needs real headroom.

---

## 3. Proposed partition layout

Two constraints shaped this:

1. **App partitions must start on a 64 KB boundary.**
2. **Moving `zb_storage` destroys the Zigbee network keys** (device must re-pair) and
   moving `history` destroys 7 days of sensor data. Since v2.1.0 is delivered by USB
   reflash anyway (§12), keeping these at their exact current offsets makes the
   migration invisible to users.

```
# AirCube Zigbee + OTA Partition Table (2 MB flash)
# Name,     Type,  SubType, Offset,   Size,     Flags
nvs,        data,  nvs,     0x9000,   0x6000,
phy_init,   data,  phy,     0xF000,   0x1000,
ota_0,      app,   ota_0,   0x10000,  0xD0000,
ota_1,      app,   ota_1,   0xE0000,  0xD0000,
zb_storage, data,  fat,     0x1B0000, 0x10000,
zb_fct,     data,  fat,     0x1C0000, 0x1000,
history,    data,  0x99,    0x1C1000, 0x11000,
otadata,    data,  ota,     0x1D2000, 0x2000,
```

| Name | Offset | Size | Changed? |
|---|---|---|---|
| `nvs` | 0x9000 | 24 KB | unchanged — brightness, auto-dim, radio flags survive |
| `phy_init` | 0xF000 | 4 KB | unchanged |
| `ota_0` | 0x10000 | **832 KB** | replaces `factory`, same start offset |
| `ota_1` | 0xE0000 | **832 KB** | new |
| `zb_storage` | 0x1B0000 | 64 KB | **unchanged — no re-pairing** |
| `zb_fct` | 0x1C0000 | 4 KB | **unchanged** |
| `history` | 0x1C1000 | 68 KB | **unchanged — history survives** |
| `otadata` | 0x1D2000 | 8 KB | new, placed in previously-unused space |
| *free* | 0x1D4000 | **176 KB** | reserve for future slot growth |

**Slot size: 0xD0000 = 851,968 bytes (832 KB).** The baseline image is 915,184, so the
app must lose 63,216 bytes (6.9 %); §4 frees 127,024, leaving **63,808 bytes of
headroom**. This layout has been **built** — the table generates cleanly and the app passes
`check_sizes.py` with rollback enabled (`0xf940 bytes (7%) free`). It has not been
flashed to hardware; that is Phase 2.

Note there is no `factory` partition. With `otadata` blank or invalid the bootloader
falls back to `ota_0`, which is exactly what a freshly flashed unit needs. Expect one
cosmetic `ESP_LOGE` on first boot — `bootloader_utility.c:463-465` logs *"ota data
partition invalid and no factory, will try all partitions"* at ERROR level. Harmless,
but worth documenting so it isn't reported as a fault.

### 3.1 Fallback: maximum-slot layout

If the firmware later outgrows 832 KB even with §4.2 applied, everything can be repacked
for the theoretical maximum:

```
nvs        0x9000   0x6000     ota_0      0x10000  0xE0000
phy_init   0xF000   0x1000     ota_1      0xF0000  0xE0000
zb_storage 0x1D0000 0x10000    zb_fct     0x1E0000 0x1000
history    0x1E1000 0x11000    otadata    0x1F2000 0x2000
```

Slots become 917,504 B (896 KB) — 64 KB more each — and `history` keeps its full 7 days,
with 48 KB still spare. The cost is that `zb_storage` moves, so **every fielded device
must re-pair**. That is a genuinely bad user experience for an already-deployed fleet,
which is why §3 is preferred while it fits. The 176 KB reserve in §3 should be spent
first: growing both slots by 64 KB there costs only the reserve, not a re-pair.

---

## 4. Getting the image under 832 KB

None of this touches application logic. All of it is `sdkconfig.defaults`.

### 4.1 Measured results

Each flag measured independently against the 915,184 B baseline:

| Change | Result | Saving | Note |
|---|---|---|---|
| `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` | 867,744 | **−47,440** | Current config is `COMPILER_OPTIMIZATION_DEBUG` (`-Og`); nothing overrides it. |
| `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` | 867,984 | **−47,200** | Nano printf. One caveat, §4.3. |
| `CONFIG_LOG_MAXIMUM_LEVEL_WARN=y` + `LOG_DEFAULT_LEVEL_WARN` | 890,064 | **−25,120** | Current level is INFO. Strips `ESP_LOGI`/`ESP_LOGD` strings. Serial JSON is `printf`, unaffected. |
| `CONFIG_ESP_ERR_TO_NAME_LOOKUP=n` | 907,008 | **−8,176** | `esp_err_to_name()` returns hex codes. |
| **All four combined** | **788,160** | **−127,024 (−13.9 %)** | **63,808 B headroom in an 832 KB slot** |

The four are close to additive — little overlap between them.

```
# Add to firmware/sdkconfig.defaults
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_LIBC_NEWLIB_NANO_FORMAT=y
CONFIG_LOG_MAXIMUM_LEVEL_WARN=y
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_ESP_ERR_TO_NAME_LOOKUP=n
```

Worth keeping a `sdkconfig.debug` overlay that restores `-Og` and
`LOG_DEFAULT_LEVEL_INFO` for development — the size config is only needed for release
builds.

### 4.2 Aggressive reserve

If more room is needed later, silent assertions and error-only logging reach
**721,216 B (−193,968, −21.2 %)**:

```
CONFIG_LOG_MAXIMUM_LEVEL_ERROR=y
CONFIG_LOG_DEFAULT_LEVEL_ERROR=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y
CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT=y
CONFIG_HAL_ASSERTION_SILENT=y
CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y
CONFIG_BT_NIMBLE_LOG_LEVEL_NONE=y
```

Not recommended by default — silent assertions and error-only logging make field
diagnosis considerably harder, and 63 KB of headroom is already adequate. This is
insurance.

### 4.3 Nano printf caveat — one code change needed

`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` is the second-largest single win but drops 64-bit
conversions (`%llu`, `%lld`, `%j`). The codebase is nearly clean already —
`serial_protocol.c:162-163` carries a comment noting that `%llu` silently fails and
casts to `unsigned long` instead — but one site remains:

```c
/* scd41.c:218-221 */
ESP_LOGI(TAG, "SCD41 initialized in single-shot mode "
              "(RH+T every %llu s, CO2 every %llu s)",
         SCD41_RHT_INTERVAL_US / 1000000ULL, SCD41_CO2_INTERVAL_US / 1000000ULL);
```

At `LOG_MAXIMUM_LEVEL_WARN` this line is compiled out, so release builds are
unaffected — but a debug build combining INFO logging with nano printf would print
garbage. Cast both to `(unsigned long)` and use `%lu`.

Float conversions (`%.2f` etc., used in `serial_protocol.c`, `auto_dim.c`, `main.c`)
**are** supported under nano format. `CONFIG_ESP_ROM_HAS_NEWLIB_NANO_FORMAT=y` on
esp32h2 is where the 47 KB comes from — the nano implementation is in ROM rather than
flash — and `CONFIG_ESP_ROM_HAS_NEWLIB_NANO_PRINTF_FLOAT_BUG=y` means IDF links a
corrected float path from flash instead of using the buggy ROM one. Verify sensor JSON
output on hardware after enabling this; it is the one change here with a functional
surface.

(For the record, the `serial_protocol.c` comment blames *picolibc*. The baseline is
`CONFIG_LIBC_NEWLIB=y` with nano off, so `%llu` works today — the comment is right about
the symptom and wrong about the cause. It stops being wrong once nano is on.)

### 4.4 What §4 costs

The size table is clean; the consequences are not, and this deserves to be a conscious
trade rather than a footnote:

- `LOG_MAXIMUM_LEVEL_WARN` permanently strips every `ESP_LOGI`/`ESP_LOGD` from release
  builds. Most of the firmware's diagnostic narrative — sensor init, model detection,
  Zigbee join progress, MTU negotiation — is `ESP_LOGI`.
- `ESP_ERR_TO_NAME_LOOKUP=n` turns the **34 `esp_err_to_name()` call sites across 11
  files** into bare hex codes.

That is most of the field-diagnosis output, removed in the same release that ships the
feature most likely to fail in the field. Two ways to soften it, either of which is
cheap: keep `LOG_MAXIMUM_LEVEL` at INFO and only lower `LOG_DEFAULT_LEVEL` (runtime
adjustable via `esp_log_level_set()`, costs back ~25 KB of the 127), or add a handful of
`ESP_LOGW` calls on the paths that matter for OTA support tickets.

Also worth a deliberate soak test: this repo pins `esp-zigbee-lib` *because of a PHY-init
timing hang* (`firmware/main/idf_component.yml:3-6`), and `-Og` → `-Os` changes code
timing throughout.
Run repeated Zigbee join/rejoin and a BLE connect/stream soak before shipping.

---

## 5. Shared OTA engine (`firmware/main/ota.c` / `ota.h`)

Both transports do the same thing to flash and differ only in how bytes arrive. One
module owns the `esp_ota_*` state machine; the Zigbee and BLE layers are thin adapters.

```c
typedef enum { OTA_SRC_ZIGBEE, OTA_SRC_BLE } ota_source_t;

/* Claim the OTA engine and open the inactive slot. Fails if busy. */
esp_err_t ota_begin(ota_source_t src, uint32_t image_size,
                    const uint8_t sha256[32] /* NULL to skip */);

/* Append bytes. Buffers internally to 4 KB before touching flash. */
esp_err_t ota_write(const void *data, size_t len);

/* Verify, set boot partition. Caller reboots. */
esp_err_t ota_commit(void);

void      ota_abort(void);
bool      ota_in_progress(void);
uint32_t  ota_bytes_received(void);
uint8_t   ota_last_error(void);      /* backs the BLE GET_STATUS opcode */

/* Rollback confirmation — call once the new image proves healthy. */
void      ota_mark_running_image_valid(void);
```

### 5.1 Erase happens up front — plan for it

`esp_ota_begin()` with a known `image_size` erases `ALIGN_UP(image_size, 4096)` bytes
**immediately** (`esp_ota_ops.c:189-197`); only `OTA_WITH_SEQUENTIAL_WRITES` defers
erasure to the write path. Erasing ~790 KB takes on the order of seconds.

Two consequences, both easy to get wrong:

- **`ota_begin()` must not run on a stack callback.** On the NimBLE host task it would
  block past a 4 s supervision timeout and drop the link; on the ZBOSS task it risks the
  stack watchdog. Post the request to the worker task, erase there, and only then send
  READY / return from the Zigbee START callback.
- Because erasure is not incremental, the 4 KB write buffer below is about **write
  efficiency and not holding up the caller**, not about hiding per-sector erase latency.

### 5.2 Responsibilities

- **4 KB write buffering.** Both transports deliver small chunks (64 B over Zigbee,
  ~180 B over BLE) on a stack callback or host-task context. `esp_ota_write()` on a
  pre-erased region is fast, but a flash transaction per 64-byte chunk is thousands of
  needless SPI round-trips on the wrong task. Accumulate, write on the boundary.
- **Header sanity check on the first 288 bytes.** Validate `esp_image_header_t.magic ==
  0xE9`, `chip_id == ESP_CHIP_ID_ESP32H2`, and the embedded `esp_app_desc_t.project_name
  == "AirCube"`. Rejecting a wrong-target or wrong-project image before writing 700 KB
  is worth the twenty lines.
- **Downgrade policy.** Compare the incoming `esp_app_desc_t.version` against
  `esp_app_get_description()->version`; refuse older unless the caller passed an
  explicit override.
- **Running SHA-256** when the transport supplied an expected hash, checked at
  `ota_commit()`. Zigbee's `.ota` container has no strong integrity field, and BLE has
  none at all, so this is the real end-to-end check. `esp_ota_end()` additionally
  validates the image's own embedded checksum.
- **Suppressing `history_check_flush()`** while in progress. Two writers on the same
  flash bus is avoidable contention, and losing one 5-minute summary is a fair trade.
  Also refuse `clear_history` during OTA.
- **LED state.** A distinct OTA indication — a slow pulse in a colour not used by air
  quality (purple/white) — and suspending `auto_dim`, so a user watching a long transfer
  knows something is happening.
- **Watchdog timeout.** No data for 30 s → `esp_ota_abort()`, free buffers, release the
  lock. Prevents a dropped connection from wedging the engine forever.
- **Blocking radio-mode switches.** `radio_mode_start_pairing()` calls `esp_restart()`
  (`radio_mode.c:83`), so a long-press mid-transfer would reboot into a half-written
  slot. `button.c` must ignore the pairing long-press while `ota_in_progress()` — ideally
  with an LED blink to say why.

### 5.3 Rollback safety

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

A new image boots in `ESP_OTA_IMG_PENDING_VERIFY`. **Do not** call
`esp_ota_mark_app_valid_cancel_rollback()` at the top of `app_main()` — that defeats the
purpose. The meaningful health check differs per mode:

- **Zigbee mode:** confirm from the signal handler once the device rejoins the network,
  ideally after the first successful attribute report. If the new image cannot rejoin,
  the watchdog reboot rolls back automatically.
- **BLE mode:** confirm once advertising is up and the sensor task has produced a valid
  reading.

**There is a trap here that must be handled explicitly.** Rollback only happens on
reboot. An image that boots and runs fine but never satisfies its health check — say a
Zigbee build that can see the network but never completes a join — sits in
`ESP_OTA_IMG_PENDING_VERIFY` indefinitely. Nothing reboots it, so nothing rolls back,
and `esp_ota_begin()` then refuses with `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`
(`esp_ota_ops.c:164-172`): **OTA is permanently dead until a USB reflash.**

So the health check needs a deadline, not just a success path. Start a timer at boot
when `esp_ota_get_state_partition()` reports `PENDING_VERIFY`; if the check has not
passed within, say, 5 minutes, call `esp_ota_mark_app_invalid_rollback_and_reboot()`.
The existing failure paths at `zigbee.c:176` and `zigbee.c:318` cover some cases but are
not a general net.

Do **not** enable `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` — it burns eFuses irreversibly
and offers little against a threat model that already permits USB reflashing.

---

## 6. Zigbee OTA client

For commissioned devices. The hub (Z2M / ZHA / SmartThings) drives the transfer;
`esp-zigbee-lib` handles the ZCL protocol and hands the application payload chunks.

### 6.1 Cluster registration

`create_cluster_list()` (`zigbee.c:444`) gains an OTA client cluster, co-located on the
existing endpoint 10 — both Z2M and ZHA discover the client cluster on any endpoint, so
Espressif's dedicated-endpoint approach isn't necessary.

```c
#define AIRCUBE_OTA_MANUFACTURER   0x6A1C     /* see §6.3 */
#define AIRCUBE_OTA_IMAGE_TYPE     0x0001     /* one image serves Base and Pro */
#define AIRCUBE_OTA_HW_VERSION     0x0001     /* PCB rev; bump for v1.2 boards */
#define AIRCUBE_OTA_MAX_DATA_SIZE  64         /* see §9 */

esp_zb_ota_cluster_cfg_t ota_cfg = {
    .ota_upgrade_file_version        = AIRCUBE_FILE_VERSION,   /* from version.txt */
    .ota_upgrade_downloaded_file_ver = AIRCUBE_FILE_VERSION,
    .ota_upgrade_manufacturer        = AIRCUBE_OTA_MANUFACTURER,
    .ota_upgrade_image_type          = AIRCUBE_OTA_IMAGE_TYPE,
    /* Two remaining members have NON-ZERO SDK defaults — zero-init would be wrong. */
    .ota_upgrade_file_offset         = ESP_ZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEF_VALUE, /* 0xffffffff */
    .ota_upgrade_server_id           = ESP_ZB_ZCL_OTA_UPGRADE_SERVER_DEF_VALUE,      /* {0xff × 8} */
    /* ota_image_upgrade_status and ota_min_block_reque default to 0; leaving them
     * zero-initialised is correct. Listed here so the omission is deliberate. */
};
esp_zb_attribute_list_t *ota = esp_zb_ota_cluster_create(&ota_cfg);

esp_zb_zcl_ota_upgrade_client_variable_t client_var = {
    .timer_query   = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,  /* 1440 min */
    .hw_version    = AIRCUBE_OTA_HW_VERSION,
    .max_data_size = AIRCUBE_OTA_MAX_DATA_SIZE,
};
uint16_t server_addr = 0xFFFF;   /* discovered */
uint8_t  server_ep   = 0xFF;

esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID,     &client_var);
esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID,     &server_addr);
esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &server_ep);
esp_zb_cluster_list_add_ota_cluster(cluster_list, ota, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
```

`AIRCUBE_FILE_VERSION` must be derived from `version.txt` at build time as
`(major << 16) | (minor << 8) | patch` — v2.0.1 → `0x00020001`. It **must** be strictly
lower than the offered `.ota` file version, or Z2M and ZHA both classify the offer as a
downgrade and skip it. Wire it through `firmware/CMakeLists.txt` so it can never drift
from `version.txt`.

Server discovery: `esp_zb_zdo_match_cluster()` for `ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE`
once the device reaches `ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT` or steering success, then
`esp_zb_ota_upgrade_client_query_image_req(addr, ep)`.

⚠️ The parameter *names* in `esp_zigbee_ota.h:121` are swapped relative to both the
doxygen and actual usage — positionally it is `(uint16_t short_addr, uint8_t endpoint)`.
Go by position, not by name.

### 6.2 The value callback

`zigbee.c`'s existing `esp_zb_core_action_handler` gains a case for
`ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID` (0x0004) that adapts onto §5. Chronological order
of `upgrade_status` is `START → RECEIVE × N → CHECK → APPLY → FINISH` (the enum values
are *not* in that order).

```c
esp_err_t ota_zigbee_value_cb(const esp_zb_zcl_ota_upgrade_value_message_t *msg)
{
    switch (msg->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        /* image_size counts the whole container. The app payload is smaller by
         * the OTA header (variable! see below) plus the 6-byte sub-element header. */
        s_app_size = msg->ota_header.image_size - ota_hdr_len(msg) - 6;
        return ota_begin(OTA_SRC_ZIGBEE, s_app_size, NULL);

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE: {
        const uint8_t *p = msg->payload; uint16_t n = msg->payload_size;
        if (!s_subelem_stripped) {                /* strip sub-element header once */
            if (n < 6 || (p[0] | (p[1] << 8)) != 0x0000) return ESP_FAIL;
            p += 6; n -= 6; s_subelem_stripped = true;   /* NOT keyed on byte count:
                              a first block of exactly 6 B would leave it at 0 */
        }
        return ota_write(p, n);
    }
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
        return (ota_bytes_received() == s_app_size) ? ESP_OK : ESP_FAIL;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        return ESP_OK;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        ESP_RETURN_ON_ERROR(ota_commit(), TAG, "commit failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        break;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
        ota_abort();
        break;
    }
    return ESP_OK;
}
```

Four things that will bite if missed:

1. **Returning anything but `ESP_OK` aborts the transfer silently.** A confirmed source
   of "OTA stops after Query Response" reports.
2. **The 6-byte sub-element header must be stripped, and the OTA header length is not a
   constant.** The stack hands you the payload *after* the OTA header but *including*
   the `[2B Tag][4B Length]` sub-element header. Reject any tag other than `0x0000`
   (Upgrade Image); flashing those 6 bytes bricks the device.

   `esp_zb_ota_file_header_t.image_size` is the size of the **whole container**
   (`esp_zigbee_ota.h:51`: *"Total image size in bytes transferred from the server to
   the client"*), so the app payload is `image_size − header_len − 6`. The OTA header is
   **56 bytes minimum but grows with optional fields** — passing `--min-hw-ver`/
   `--max-hw-ver` to the image builder (as §8.1 does, and as you want for hardware
   gating) sets the hardware-versions bit and makes it 60.

   The SDK gives you no help here. `esp_zb_ota_file_header_t` has **no `header_length`
   member**, the app never sees the raw header bytes (`payload` starts after it), and
   1.6.7 ships no field-control bit macros. Derive it from `field_control` and the ZCL
   spec bit values yourself:

   ```c
   static uint16_t ota_hdr_len(const esp_zb_zcl_ota_upgrade_value_message_t *m)
   {
       uint16_t fc = m->ota_header.field_control;
       return 56 + ((fc & 0x0001) ? 1 : 0)    /* security credential version */
                 + ((fc & 0x0002) ? 8 : 0)    /* upgrade file destination (EUI64) */
                 + ((fc & 0x0004) ? 4 : 0);   /* min + max hardware version */
   }
   ```

   Hardcoding 56 silently corrupts every transfer that uses hardware gating.
3. **`msg->payload` is borrowed** — valid only for the duration of the callback.
4. **The callback runs on the ZBOSS task**, which is why §5 buffers rather than writing
   through.

### 6.3 Manufacturer code

StuckAtPrototype has no CSA-assigned manufacturer code. Options, best first:

1. **Pick an unassigned 16-bit value** and use it consistently. Verify the candidate is
   absent from `zigbee-herdsman`'s `manufacturerCode.ts` (723 entries). There is
   established DIY precedent — herdsman carries an explicit *"Non-CSA-assigned codes"*
   block (`CUSTOM_SPRUT_DEVICE = 0x6666`, `CUSTOM_LYTKO = 0x7777`). Avoid 0xFFF5–0xFFFF
   (reserved) and, notably, **do not copy Espressif's example value 0x1001** — that is
   CHIPCON's assigned code.
2. **CSA test codes 0xFFF1–0xFFF4** — spec-blessed but semantically "not for shipping
   product".
3. Join the CSA. Not proportionate here.

`0x6A1C` is used throughout this document as a concrete placeholder; it was verified
absent from `zigbee-herdsman`'s `manufacturerCode.ts` as of August 2026. Re-check before
committing to it, and change it in one place (§6.1) if the maintainer prefers another.

Neither Z2M nor ZHA validates against the CSA registry; they match purely on
`(manufacturerCode, imageType)` from the OTA header. The real risk of an ad-hoc code is
**collision in the public index** — if the pair matches a real vendor's entry in
`Koenkk/zigbee-OTA/index.json`, Z2M could offer their firmware to an AirCube. Mitigate
by picking a distinctive pair *and* always setting `modelId` / `manufacturerName`
constraints in the index entry.

---

## 7. BLE OTA transport

For uncommissioned devices — which is every device out of the box, and any device whose
owner uses the companion app rather than a hub. It is also far faster than the Zigbee
path — roughly **75×** against Z2M's out-of-the-box settings, or **12×** against a tuned
Z2M (§9) — so it is the better experience wherever it is available.

Because `radio_mode.c` starts exactly one stack per boot, BLE OTA and Zigbee OTA can
never run concurrently; no cross-transport arbitration is needed.

### 7.1 New characteristics

Two additions to the existing service, following the base UUID suffix
`-1D0F-4E7C-8E4B-2A3D5F6B7C80`:

| UUID prefix | Name | Properties | Size |
|---|---|---|---|
| `A17C0DE6` | OTA Control | Write, Notify | 41 B write / 8 B notify |
| `A17C0DE7` | OTA Data | **Write Without Response** | ≤ MTU−3 |

Write Without Response on the data characteristic is the whole point. An ATT Write
Request round-trips once per connection interval (~15–30 ms), capping throughput near
6–12 KB/s. Write Without Response lets the client push several PDUs per interval,
reaching 15–25 KB/s on iOS. Flow control moves to the application layer (§7.3).

These go in the nested `.characteristics` array at `ble_gatt.c:577-608` — *not* in the
top-level `gatt_svcs[]` at `ble_gatt.c:573`, which is an array of services — inserted
before the `{ 0 }` terminator alongside the existing five:

```c
{
    .uuid       = &UUID_OTA_CTRL.u,
    .access_cb  = ota_ctrl_access_cb,
    .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
    .val_handle = &s_ota_ctrl_val_handle,
},
{
    .uuid      = &UUID_OTA_DATA.u,
    .access_cb = ota_data_access_cb,
    .flags     = BLE_GATT_CHR_F_WRITE_NO_RSP,
},
```

### 7.2 Control protocol

**Client → device** (write to `A17C0DE6`):

| Opcode | Name | Payload |
|---|---|---|
| 0x01 | BEGIN | `u32 image_size`, `u8[32] sha256`, `u8 major/minor/patch`, `u8 flags` |
| 0x02 | COMMIT | — |
| 0x03 | ABORT | — |
| 0x04 | GET_STATUS | — |

`flags` bit0 = allow downgrade. BEGIN is 41 bytes (1 + 4 + 32 + 3 + 1), which needs
`MTU ≥ 44`. ATT has no fragmentation — a shorter MTU would force a Prepare/Execute long
write, which NimBLE only supports with queued-write buffers configured. Simpler to
require a minimum: if `ble_att_mtu() < 64`, reply ERROR/bad-request. (Below 44 the BEGIN write could not
arrive intact to be rejected at all, so this is a guard against the awkward middle
band rather than a complete one.) iOS and Android both negotiate ≥ 185 in practice.

**Device → client** (notify on `A17C0DE6`), 8 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | frame type |
| 1 | u8 | status / error reason |
| 2 | u32 | bytes_received |
| 6 | u16 | credit (chunks the client may send now) |

Frame types: `0x01 READY`, `0x02 CREDIT`, `0x03 DONE`, `0x04 ERROR`.

Error reasons: `1 busy` (a history stream is active, or an OTA is already running),
`2 bad request`, `3 image too large`, `4 bad image header`, `5 downgrade refused`,
`6 hash mismatch`, `7 flash write failed`, `8 timeout`.

### 7.3 Flow control — credit windowing

Write Without Response has no ATT-level acknowledgement, so the device grants credits,
in the manner of Nordic's DFU packet-receipt-notification scheme:

1. Client writes BEGIN. Device claims the engine, posts to the worker task, which erases
   `ALIGN_UP(image_size, 4096)` bytes of the target slot (§5.1 — seconds) and only then
   notifies READY with the effective chunk size
   and an initial `credit`.
2. Client writes up to `credit` chunks to `A17C0DE7`.
3. Device replenishes credit **as chunks land in the ring buffer**, not after they reach
   flash. Two windows of buffer mean the client can keep filling window *n+1* while the
   worker drains window *n*, so transfer and flash writes overlap instead of alternating.
4. Repeat until `bytes_received == image_size`, then client writes COMMIT.
5. Device verifies, notifies DONE, and reboots after ~1 s.

Credit is therefore backpressure on the *ring buffer*, not on flash — it exists so a
fast client cannot overrun a busy worker. Grant one window (4 KB) at READY, then a fresh
window each time the worker frees one. At MTU 185 a chunk carries 180 payload bytes, so
**credit = 22** chunks = 3,960 B per window, and the ring buffer wants 2 × 4,096 B.

Stop-and-wait — replenishing only after flash — is simpler and worth building first, but
it serialises transfer against flash and will land below the §9.1 throughput figures.
Treat overlapped credit as the target and stop-and-wait as the fallback if it misbehaves.

Each Data write is `[u16 seq][payload]`. BLE preserves ordering within a connection and
the link layer retransmits, so loss is rare — but a client stack under memory pressure
can drop a write-without-response, and two bytes of sequence number turn a silent
corruption into a clean ERROR carrying the expected `seq`. The device *validates* the
client's counter (it does not maintain its own). This also makes §7.7 resume possible.

### 7.4 Task structure

Mirror the existing history streaming design rather than inventing a new one. Flash
writes must not happen on the NimBLE host task.

- `ota_data_access_cb()` runs on the host task: `ble_hs_mbuf_to_flat()` into a ring
  buffer, bump `seq`, return. Nothing blocking.
- A dedicated `ble_ota_task` (4096 B stack, priority 4 — same as `ble_hist_stream`)
  drains the ring buffer into `ota_write()` and emits CREDIT notifications via the
  existing `notify_with_retry()`.
- The ring buffer needs roughly two windows, ~8 KB, allocated at BEGIN and freed at
  COMMIT/ABORT. The nearest precedent is `serial_protocol.c:200`, which transiently
  `malloc()`s a 12 KB history page buffer — so an 8 KB transient allocation is within
  the envelope the firmware already tolerates. (`history.c` itself never allocates; it
  reads a slot at a time.) Nothing in `firmware/main` currently logs free heap, so
  **measure it before committing to the size** — add an `esp_get_free_heap_size()` trace
  in BLE mode, then check the number at BEGIN and reply ERROR/busy rather than crashing.

### 7.5 Concurrency with history streaming

Reuse the device-wide `history_stream_acquire()` lock that BLE and USB serial already
share. BEGIN fails with ERROR/busy if a history stream is running; history stream
requests fail with the existing `ERR_BUSY` while OTA is in progress. One bulk transfer
at a time, device-wide, which is already the established rule.

### 7.6 Connection parameters and MTU

At BEGIN, request a faster connection interval:

```c
struct ble_gap_upd_params p = {
    .itvl_min = 12, .itvl_max = 24,     /* 15–30 ms */
    .latency = 0, .supervision_timeout = 400,
};
ble_gap_update_params(s_conn_handle, &p);
```

iOS may clamp these; request anyway and restore the defaults after COMMIT or ABORT. The
firmware already prefers MTU 256 (`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256`) and iOS
typically negotiates 185; read the effective value with `ble_att_mtu()` and report the
resulting chunk size in the READY frame so the client never has to guess.

### 7.7 Optional: resumable transfers

BLE range is short and users walk away. A worthwhile v2 refinement: on disconnect, hold
the partial OTA state for 60 s instead of aborting immediately. A subsequent BEGIN
carrying the same `sha256` and `image_size` resumes from `bytes_received` rather than
restarting. Cheap in flash terms (nothing extra is written), and it turns a failed
80-second transfer into a 20-second one. Not required for the first release.

### 7.8 Security — needs a decision

`CONFIG_BT_NIMBLE_SECURITY_ENABLE` is off and there is no bonding, so **any BLE central
in radio range could push firmware** once this endpoint exists. That is a meaningful
change to the device's threat model and should be a deliberate choice, not a
side-effect. Three options:

**1. Signed images (recommended).**

```
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="ota_signing_key.pem"
```

On ESP32-H2 the scheme is **RSA-3072**, not ECDSA — H2 has
`SECURE_BOOT_V2_RSA_SUPPORTED` and no Secure Boot V1, so IDF selects
`SECURE_SIGNED_APPS_RSA_SCHEME` automatically. (`SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT`,
which would make the *bootloader* check at every boot, requires the **Secure Boot V1**
ECDSA scheme, which H2 does not have. Verification happens on the OTA-update path only, inside
`esp_ota_end()` / `esp_image_verify()`. That is the path that matters here.)

The trust anchor is worth understanding, because it is unusual and it is what makes this
work without burning eFuses. With Secure Boot disabled,
`get_secure_boot_key_digests()` (`secure_boot_signatures_app.c`) takes the trusted key
digests **from the signature block of the currently running app** rather than from
eFuse. So the chain is: whatever key signed the running image must have signed the
update. It is bootstrapped by the v2.1.0 USB flash and self-perpetuating from there.

Properties that follow:

- A BLE or network attacker cannot forge an update without the private key. This is real
  protection, not theatre.
- Nothing is irreversible. No eFuses are burned; a lost or rotated key is recovered by a
  USB reflash, which bypasses the check entirely.
- **It must ship in v2.1.0.** Enabling it in a later OTA-delivered release would mean
  the running (unsigned) app has no signature block to act as the anchor — costing a
  second mandatory USB flash. This is the reason §11 puts the decision in Phase 1.
- It protects the Zigbee path equally, which otherwise trusts whatever the hub offers.

**Measured cost: +35,136 bytes** (788,160 → 823,296), leaving 28,672 bytes of headroom
in the 832 KB slot. That is real but affordable; it does mean the §4.2 reserve is more
likely to be needed sooner. The other cost is release discipline — the private key must
stay out of the repo and be backed up, which is a genuine ongoing burden for a community
project and the main argument against.

**2. Physical confirmation.** Require a button press within N seconds of BEGIN before
the device accepts data. Free, no key management, consistent with how pairing already
works — but it rules out unattended updates, and it is no obstacle to someone who
already has physical access.

**3. Accept the risk.** Defensible for a hobbyist device: anyone in BLE range could
equally unplug it and use ESP Launchpad. If this is the choice, state it explicitly in
the docs rather than leaving it implicit.

Option 1 is the recommendation, with option 2 as an optional extra for BLE. Whatever is
picked, note that `esp_ota_end()` already rejects images whose internal checksum fails
and §5 rejects wrong-chip and wrong-project images — so the exposure is to a
*deliberately crafted valid* AirCube image, not to accidental corruption.

### 7.9 Client-side and protocol documentation

The iOS client lives in a separate repository, so `docs/BLE_GATT_PROTOCOL.md` is the
contract and must be updated in the same change:

- Use the currently-unused `reserved` byte at offset 5 of Device Info as a capability
  bitmask, bit0 = "OTA supported". The fixed 14-byte layout is preserved and old clients
  that ignore the byte keep working.
- **Leave the `protocol_version` wire byte at 1.** It is a real field
  (`ble_gatt.c:87`, documented as `= 1` in `BLE_GATT_PROTOCOL.md:44`) and existing
  clients may validate it; bumping it would break exactly the compatibility the
  capability bit is there to preserve. Version the *document* v2 and let clients feature-
  detect from the capability bit. If a future change is genuinely incompatible, bump the
  byte then.
- Document the two characteristics, the control opcodes, the credit scheme, and the
  error reasons.

The Python desktop app (`scripts/`) talks USB serial, not BLE, so it needs no change
for this.

---

## 8. Build and release tooling

### 8.1 Generating the `.ota` file

Espressif ships `tools/image_builder_tool/image_builder_tool.py` in esp-zigbee-sdk (a
thin wrapper over `zigpy`). **Vendor a copy pinned to the 1.6.x-era commit
`4cab73fe`** — the CLI changed incompatibly in 2.0 (`--create`/`--tag-file` replaced by
`--tag TAG_ID:TAG_FILE`), and an unpinned copy will silently break the release script.

```bash
python image_builder_tool.py \
  --create   AirCube_v2.2.0.ota \
  --manuf-id 0x6A1C \
  --image-type 0x0001 \
  --version  0x00020200 \
  --stack-version 2 \
  --header_string "AirCube v2.2.0" \
  --min-hw-ver 0x0001 --max-hw-ver 0x0001 \
  --tag-id 0x0000 --tag-file build/AirCube.bin
```

Requires `pip install zigpy`. Header layout: 4B identifier `0x0BEEF11E`, 2B header
version `0x0100`, 2B header length, 2B field control, 2B manufacturer code, 2B image
type, 4B file version, 2B stack version (`0x0002` = Zigbee Pro), 32B header string, 4B
total image size — 56 bytes — **plus 4 bytes of min/max hardware version when
`--min-hw-ver` is passed**, as above. The command therefore produces a 60-byte header
and a `.ota` of `60 + 6 + len(.bin)` = 788,226 B for a 788,160 B app. Never hardcode
either header length (§6.2).

The BLE path consumes the raw `.bin` directly — no container — with the SHA-256 carried
in the BEGIN command. Publish that hash alongside the release so clients can verify what
they downloaded.

### 8.2 Release artifacts

| Artifact | Purpose |
|---|---|
| `AirCube_firmware_vX.Y.Z.bin` | full-flash image at 0x0 for ESP Launchpad (existing) |
| `AirCube_app_vX.Y.Z.bin` | **app-only image for BLE OTA** (new) |
| `AirCube_app_vX.Y.Z.bin.sha256` | hash for the BLE BEGIN command |
| `AirCube_vX.Y.Z.ota` | Zigbee OTA image |
| `index.json` | Z2M-format OTA index |
| `zigpy_index.json` | zigpy/ZHA-format index |
| `AirCube_vX.Y.Z.elf` + `.map` | **archive these** — needed for delta OTA (§10.2) and crash decoding |

### 8.3 Hosting the index

Z2M and zigpy use different index schemas; generate both.

**Z2M** (`index.json`):

```json
[{
  "fileName": "AirCube_v2.2.0.ota",
  "url": "https://github.com/StuckAtPrototype/AirCube/releases/download/v2.2.0/AirCube_v2.2.0.ota",
  "fileVersion": 131584,
  "fileSize": 788226,
  "manufacturerCode": 27164,
  "imageType": 1,
  "sha512": "...",
  "otaHeaderString": "AirCube v2.2.0",
  "modelId": "AirCube",
  "manufacturerName": ["StuckAtPrototype"],
  "minFileVersion": 131328,
  "releaseNotes": "..."
}]
```

**zigpy/ZHA** (`zigpy_index.json`) — generate with
`zigpy ota generate-index --ota-url-root=<url> *.ota`; mandatory fields are
`file_version`, `file_size`, `image_type`, `manufacturer_id`, `checksum`, plus
`manufacturer_names` / `model_names` for matching.

Long term, submit images upstream to [`Koenkk/zigbee-OTA`](https://github.com/Koenkk/zigbee-OTA)
and [`zigpy/zigpy-ota`](https://github.com/zigpy/zigpy-ota) so users get updates with
zero configuration. Until then, users add an override.

### 8.4 Integration docs to update

**`z2m/aircube.mjs`** — add `ota: true` to the definition. The old
`import * as ota from 'zigbee-herdsman-converters/lib/ota'` / `ota: ota.zigbeeOTA`
pattern is **obsolete**; that module no longer exists (`Definition.ota` is now
`boolean | {...}` in `src/lib/types.ts`). Note the Z2M external-converters page still
shows the old import, so expect to second-guess this. Also note in `README.md` that
external converters require `enable_external_js: true` — disabled by default for new
Z2M installations from 2.11.0; existing installs are unaffected.

User-side Z2M `configuration.yaml`:

```yaml
ota:
    zigbee_ota_override_index_location: https://.../index.json
    image_block_response_delay: 50      # default 250 — see §9
    default_maximum_data_size: 100      # default 50
```

**`zha/aircube.py`** — ZHA quirks are irrelevant to OTA (the update entity comes from
the provider, not the quirk), but document the provider config:

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: zigpy_remote
          url: https://.../zigpy_index.json
          manufacturer_ids: [0x6A1C]
```

**`smartthings/`** — the Edge driver channel handles firmware separately; out of scope
for the first pass.

---

## 9. Transfer time

The two transports are not remotely comparable, which is worth communicating to users.

### 9.1 BLE

At MTU 185 with Write Without Response and a 15–30 ms connection interval, iOS
realistically sustains 15–25 KB/s:

| Image | at 15 KB/s | at 25 KB/s |
|---|---|---|
| 788 KB | ~53 s | ~32 s |

Fast enough that a progress bar and "keep the app open" is the entire UX.

### 9.2 Zigbee

Espressif recommends `max_data_size = 223`, but **Z2M caps it at
`default_maximum_data_size`, which defaults to 50 bytes**, and inserts
`image_block_response_delay` (default 250 ms) between blocks — a ceiling of 200 B/s.

Effective block size is `min(device max_data_size, Z2M default_maximum_data_size)`, so
with the device pinned at 64 the "tuned" column below is 64 B / 50 ms, not 100 B:

| App image | Z2M defaults (50 B / 250 ms) | Tuned (64 B / 50 ms) |
|---|---|---|
| 915,184 B (baseline) | ~76 minutes | ~12 minutes |
| **788,160 B (post-§4)** | **~66 minutes** | **~10 minutes** |
| 721,216 B (§4.2) | ~60 minutes | ~9.4 minutes |

Espressif's own ESP↔ESP measurement was 618 KB in 703 s (~0.88 KB/s) unoptimised and
~8.1 KB/s after tuning `max_data_size`, `CONFIG_FREERTOS_HZ`,
`CONFIG_IEEE802154_TIMING_OPTIMIZATION` and log levels — lab numbers between two ESP
devices; a real mesh will be slower.

Set `max_data_size = 64` rather than 223: Espressif staff recommend 64 for noisy
environments so blocks never fragment, and Z2M caps below that anyway unless the user
tunes their config.

**Docs guidance:** tell users a hub-driven update takes 10–80 minutes depending on their
Z2M settings and mesh, that the device stays functional throughout, and that they should
update one device at a time. If they have the app, BLE is dramatically faster.

### 9.3 Known Zigbee SDK weakness

`esp-zigbee-lib` 1.6.x has **no per-block retry**. A single `ImageBlockResponse` timeout
resets `FileOffset` to 0 and `ImageUpgradeStatus` to `NORMAL` — no retry, no ABORT
callback, the transfer silently restarts. With 15,764 blocks at Z2M's default 50-byte
size (18,304 for the unshrunk image), the odds of a clean run on a marginal link are poor.

Mitigations, in order:

1. **Shrink the image** (§4) — fewer blocks, fewer chances to fail.
2. **Recommend users raise `default_maximum_data_size` to 100** and lower
   `image_block_response_delay` to 50 ms. Note the device's own `max_data_size = 64`
   caps this — a 22 % reduction in block count, not 50 % — so if the field data justifies
   it, raising the device value to 100 as well is the bigger lever.
3. Zigbee TX power is already at the H2 maximum (20 dBm); keep it there.
4. If field testing shows repeated restarts, the escape hatch is
   `esp_zb_raw_command_handler_register()` and handling cluster 0x0019 command 0x03
   directly. Significant work — contingency, not plan.

This is the largest technical risk in the project and should be validated early (§11
phase 4) against a real Z2M instance, not an ESP-to-ESP rig. Note that the BLE path has
no equivalent weakness, which is another argument for shipping it first.

---

## 10. Alternatives considered

### 10.1 4 MB module (ESP32-H2-MINI-1-H4 or -N4)

The `-H4` (4 MB, −40…105 °C) and `-N4` (4 MB, −40…85 °C) are pin-compatible drop-in
replacements for the current `-H2`. A single BOM line change gives 1.5 MB slots and
retires the whole flash-budget question.

It does nothing for fielded 2 MB units, so it is complementary rather than an
alternative — but worth putting on the v1.2 PCB revision regardless. The firmware would
then need to handle two partition layouts, or ship the 2 MB layout on 4 MB hardware and
leave the space unused.

### 10.2 Delta OTA

`espressif/esp_delta_ota` (with `CONFIG_ZB_DELTA_OTA`) transfers a binary patch instead
of a full image: read the running partition, apply the patch, write the target
partition. It **does not remove the need for a second slot**, so it is no help with
flash — but at roughly 10× reduction in transfer size it is a strong answer to §9.2,
cutting a 66-minute Zigbee update to a few minutes.

The cost is release discipline: patches are generated against a specific base binary
(`esp_delta_ota_patch_gen.py --base_binary <shipped> --new_binary <new>`), so **every
shipped binary must be archived**, and each release needs one patch per supported prior
version. Worth adopting in a second pass, once full-image OTA is proven — and mainly for
the Zigbee path, since BLE is already fast enough.

### 10.3 USB serial OTA

Adding `ota_*` commands to `serial_protocol.c` was considered and rejected. ESP
Launchpad already covers USB flashing well, the JSON line protocol would need base64
framing for binary payloads, and the recovery story is identical. No user benefit.

---

## 11. Phased plan

**Phase 0 — measure. ✅ Done.**
Baseline 915,184 B; §4 config 788,160 B; 823,296 B signed. The §3 layout builds and
passes `check_sizes.py` with 63,808 B free (28,672 signed). Nothing has been flashed —
"builds and fits", not "confirmed working". §3.1 is not needed.

**Phase 1 — decide on §7.8. Blocking.**
Signed images, button confirmation, both, or neither. Signing must be settled first
because it has to ship in the v2.1.0 USB flash to have a trust anchor, and because it
consumes 35 KB of the 63 KB headroom — which in turn affects whether §4.2 is needed.

**Phase 2 — flash layout.**
New `partitions_zigbee.csv` per §3, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, the §4
flags, and the `scd41.c` `%llu` fix from §4.3. Verify on hardware: sensor JSON still
formats floats correctly under nano printf; a manually-flashed `ota_1` image boots;
rollback fires when that image is made to fail. No OTA yet — this phase is purely "does
the device survive a slot swap".

**Phase 3 — `ota.c` engine + BLE transport.**
BLE first: it is the faster path, it has no SDK landmines, and it is far easier to
iterate against than a hub. Build §5 and §7 together, validate end-to-end with a test
client, measure real throughput, exercise abort/disconnect/timeout, and confirm rollback
on a deliberately broken image.

**Phase 4 — Zigbee transport.**
§6 on top of the now-proven engine. Validate first against Espressif's
`esp_zigbee_ota/ota_server` example, **then against a real Z2M instance** — §9.3 only
shows up on the latter. Measure actual transfer time at Z2M defaults.

**Phase 5 — release tooling.**
Vendored `image_builder_tool.py` at commit `4cab73fe`, version encoding wired from
`version.txt` through CMake, app-only `.bin` + SHA-256 artifacts, `index.json` +
`zigpy_index.json` generation, `.elf`/`.map` archival.

**Phase 6 — integrations and docs.**
`ota: true` in `z2m/aircube.mjs`; ZHA provider config; `docs/BLE_GATT_PROTOCOL.md`
bumped to v2 per §7.9; `FIRMWARE_UPDATE.md` rewritten around three paths (app / hub /
USB) with an explicit callout that v2.1.0 requires a one-time USB flash.

**Phase 7 — v2.1.0 release.**
Ship as USB-only. It is the last mandatory USB flash. Then cut a trivial v2.1.1 purely
to exercise both OTA paths end-to-end on real hardware before anything substantial rides
on them.

**Later:** delta OTA (§10.2), 4 MB on the v1.2 PCB (§10.1), resumable BLE transfers
(§7.7).

---

## 12. The migration constraint

**Fielded v2.0.1 devices cannot receive OTA.** Their partition table has a single
`factory` partition; there is no `otadata`, no second slot, and the running app occupies
the flash region where both new slots would live. Rewriting the partition table at
0x8000 from the running app is technically possible, but a power cut mid-write is an
unrecoverable brick — not acceptable for a shipped device.

So v2.1.0 **must** be delivered over USB via ESP Launchpad, exactly as today. The §3
layout is designed to make that migration invisible: `nvs`, `zb_storage`, `zb_fct` and
`history` all keep their current offsets, so brightness settings, Zigbee pairing and
7 days of sensor history all survive. The v2.1.0 release image must be a **full-flash
image at 0x0** including the new bootloader, partition table, `ota_0` payload and a
blank `otadata` — the existing FIRMWARE_UPDATE.md flow already flashes at 0x0, so the
procedure is unchanged.

Two practical notes for the release notes. The download grows: a merged image spanning
0x0–0x1D4000 is ~1.83 MB against today's 945 KB, so flashing takes roughly twice as
long. And `otadata` should be **explicitly included as erased (0xFF) bytes** rather than
left out — the region at 0x1D2000 was merely unallocated in the old layout, so its
contents are not guaranteed, and a stale non-0xFF pattern there would be interpreted as
a boot selection.

From v2.1.0 onward, updates arrive through the app or the hub.

---

## Appendix: reproducing the measurements

All figures come from builds of `master` (f9ec414) against ESP-IDF **v5.5.1**, target
esp32h2, with `esp-zigbee-lib` 1.6.7 / `esp-zboss-lib` 1.6.4 resolved by the component
manager. Sizes are `build/AirCube.bin` — the app partition payload, not the merged flash
image.

```bash
idf.py set-target esp32h2

# baseline
idf.py -B build_base build && stat -c%s build_base/AirCube.bin      # 915184

# one flag at a time (repeat per row of the §4.1 table)
printf 'CONFIG_COMPILER_OPTIMIZATION_SIZE=y\n' > extra.defaults
idf.py -B build_x -D SDKCONFIG=sdkconfig.x \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;extra.defaults" build
stat -c%s build_x/AirCube.bin

# where the bytes are
idf.py -B build_base size-components
```

The §3 partition table was verified by building with
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_ota.csv"` and
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` alongside the §4 flags; the table generates
cleanly and the 788,160 B app passes the 832 KB size check.

---

## References

- [ESP Zigbee SDK — ZCL OTA Upgrade Cluster](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32h2/user-guide/zcl_ota_upgrade.html) (documents the 2.0 `ezb_*` API; no versioned 1.6.x docs are published — use the headers and the pinned example below)
- [`ota_client` example @ 4cab73fe](https://github.com/espressif/esp-zigbee-sdk/blob/4cab73fed37cb8fe4150d0b4fa180031ca30c16a/examples/esp_zigbee_ota/ota_client/main/esp_ota_client.c) — the 1.x-era reference implementation
- [`image_builder_tool.py` @ 4cab73fe](https://github.com/espressif/esp-zigbee-sdk/blob/4cab73fed37cb8fe4150d0b4fa180031ca30c16a/tools/image_builder_tool/image_builder_tool.py)
- [esp-zigbee-lib 1.6.7 component](https://components.espressif.com/components/espressif/esp-zigbee-lib) — authoritative headers for the pinned version
- [ESP-IDF OTA API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html) · [Signed app verification without Secure Boot](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/security/secure-boot-v2.html) · [esp_delta_ota](https://components.espressif.com/component/espressif/esp_delta_ota)
- [Apple — Core Bluetooth best practices](https://developer.apple.com/library/archive/qa/qa1931/_index.html) (write-without-response throughput, connection interval expectations)
- esp-zigbee-sdk issues: [#833 no block retry](https://github.com/espressif/esp-zigbee-sdk/issues/833) · [#763 server offset drift](https://github.com/espressif/esp-zigbee-sdk/issues/763) · [#588 ESP_FAIL aborts OTA](https://github.com/espressif/esp-zigbee-sdk/issues/588) · [#718 bufpool assert during OTA](https://github.com/espressif/esp-zigbee-sdk/issues/718)
- [Zigbee2MQTT OTA updates](https://www.zigbee2mqtt.io/guide/usage/ota_updates.html) · [OTA configuration](https://www.zigbee2mqtt.io/guide/configuration/ota-device-updates.html) · [External converters](https://www.zigbee2mqtt.io/advanced/more/external_converters.html)
- [Koenkk/zigbee-OTA index schema](https://github.com/Koenkk/zigbee-OTA) · [zigbee-herdsman manufacturerCode.ts](https://github.com/Koenkk/zigbee-herdsman/blob/master/src/zspec/zcl/definition/manufacturerCode.ts)
- [zigpy OTA configuration](https://github.com/zigpy/zigpy/wiki/OTA-Configuration) · [OTA info for manufacturers](https://github.com/zigpy/zigpy/wiki/OTA-Information-for-Manufacturers) · [zigpy-ota](https://github.com/zigpy/zigpy-ota)
- [ESP32-H2-MINI-1 datasheet](https://documentation.espressif.com/esp32-h2-mini-1_mini-1u_datasheet_en.html) — `-H2` = 2 MB, `-H4`/`-N4` = 4 MB, pin-compatible
