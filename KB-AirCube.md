# AirCube Knowledge Base
**Owner:** StuckAtPrototype
**Stability:** Stable

## 🚀 1. CORE RESPONSIBILITY
AirCube is a desktop air quality monitor that measures temperature, humidity, CO2, TVOC, and AQI — displaying air quality as a glanceable LED color and reporting all readings to Home Assistant over Zigbee. It operates standalone out of the box with no Wi-Fi, cloud, or accounts required.

## 🧩 2. ARCHITECTURAL CONTEXT

### System Map
```
┌─────────────┐     ┌─────────────┐
│   ENS210    │────►│  ESP32-H2   │────► USB-Serial (JSON)
│ (Temp/Hum) │     │   (MCU)    │          │
└─────────────┘     └──────┬──────┘    ┌─► History (flash)
                           │           │      └─► Zigbee (802.15.4)
┌─────────────┐            ▼           │
│  ENS16X     │────► Air Quality          │
│ (Air Qual) │────► (eCO2/eTVOC/AQI)    │
└─────────────┘                        ▼
                                      ┌──────┐
                              LED (WS2812 x3) ──► Visual
                                      └──────┘
```

### Components
| Layer | Component | Function |
|-------|-----------|----------|
| MCU | ESP32-H2-MINI-1 | Main processor, Zigbee radio |
| Temp/Humidity | ENS210 | I2C temperature + humidity sensor |
| Air Quality | ENS16X | I2C eCO2, eTVOC, AQI sensor |
| Display | WS2812 x3 | RGB LED ring for AQI visualization |
| Storage | Flash (ring buffer) | 7-day sensor history |
| Button | Tactile switch | Brightness control, pairing mode |
| Connectivity | USB-C | Power + serial data |
| Wireless | Zigbee (802.15.4) | Home Assistant integration |

### Data Flow
1. sensor_task (1s period): reads ENS210 → reads ENS16X → computes AQI
2. Sends JSON over USB-Serial
3. Records min/avg/max to flash every 5 minutes
4. Pushes to Zigbee cluster 0xFC01
5. main_loop (20ms): updates LED color based on AQI

### External Integrations
| Integration | Protocol | Custom Cluster |
|-------------|----------|----------------|
| ZHA (Home Assistant) | Zigbee ZCL | 0xFC01 (eCO2, eTVOC, AQI) |
| Zigbee2MQTT | MQTT | 0xFC01 (converter) |

## ⚙️ 3. TECHNICAL SPECIFICATION

### Stack
| Layer | Technology |
|-------|------------|
| Firmware | ESP-IDF v5.0+, FreeRTOS |
| Language | C (firmware), Python 3 (desktop scripts) |
| Desktop GUI | PyQt6, Matplotlib |
| Build | CMake (firmware), PyInstaller (exe) |
| Hardware Design | KiCad |
| Enclosure | STEP (3D printable) |

### Host-Side Tests
| Test | Location | Command |
|------|----------|---------|
| led_color_lib | tests/test_led_color_lib.py | `python -m unittest discover -s tests` |
| serial parser | — | **Missing** |
| history module | — | **Missing** |

### Build Commands

**Firmware:**
```bash
cd firmware
idf.py set-target esp32h2
idf.py build
idf.py -p PORT flash monitor
```

**Desktop App:**
```bash
cd scripts
pip install -r requirements.txt
python aircube_app.py
```

**Executable:**
```bash
python build_exe.py     # → dist/AirCube.exe
python build_tray.py    # → dist/AirCubeTray.exe
```

### Serial Protocol
- Baud: 115200
- Format: Single-line JSON terminated by `\n`
- Commands: `get_config`, `set_intensity`, `set_readout_period`, `get_history`, `clear_history`

### Config Locations
- Firmware config: `firmware/sdkconfig.defaults`
- Zigbee TX power: `CONFIG_AIRCUBE_ZB_TX_POWER_DBM` (default: 10 dBm)
- History: Flash partition (2016 entries, 32 bytes/entry)
- Desktop config: Runtime (in-memory)

## 🛡️ 4. DATA & SECURITY (The "Guardrails")

### Data Repositories
| Data | Storage | Location | Retention |
|------|---------|----------|-----------|
| Sensor history | Flash ring buffer | Flash partition | 7 days |
| Config | NVS | ESP32 flash | Persistent |
| Serial output | RAM | USB-Serial | Real-time only |

### Security Patterns
| Pattern | Status | Notes |
|---------|--------|-------|
| No hardcoded credentials | ✅ | None found in codebase |
| No cloud connectivity | ✅ | Standalone or Zigbee only |
| No API keys | ✅ | Not required |
| Zigbee network key | Managed by coordinator | Not stored in device |

### Operational Notes
- **Critical Bug in Main Repo:** The public ZHA quirk (`zha/aircube.py`) incorrectly declares `is_manufacturer_specific=True` for eCO2, eTVOC, AQI (lines 19-21). The PUBLIC firmware source (`firmware/main/zigbee.c:355`) ALSO sends `.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC` — so this bug affects ALL firmware builds, not just custom_patch. Result: sensors show as "Unknown" in ZHA. **Fix:** Set `is_manufacturer_specific=False` in `zha/aircube.py`. Zigbee2MQTT (`z2m/aircube.js`) is unaffected.
- **AQI Scale:** Public firmware sends only AQI-S (0-500 range) over Zigbee (`zigbee.c:483`), not AQI-UBA (1-5). Serial JSON (`serial_protocol.c:65`) outputs both. Custom dashboard correctly bands on AQI-S.
- **Custom Dashboard:** `custom_patch/v1.3_2MB/homeassistant/www/aircube-dashboard-card.js` provides a Lovelace card with auto-discovery, live sparklines, and ENS161 AQI-S banding. Install to `/config/www/` and add as `type: custom:aircube-dashboard-card`.
- **Firmware Branch Note:** The public repo **does** have Zigbee source (`firmware/main/zigbee.c`). The claim in `custom_patch/README.md` that "zigbee.c did not appear in the public dev branch" is **incorrect** — this should be corrected or removed.

### Credential Matrix
| Item | Type | Location | Exposure |
|------|------|----------|-----------|
| Zigbee network key | External | Coordinator (HA) | Network-local |
| None device-side | N/A | N/A | N/A |

### Invariants
- Sensor warm-up time: ~3 minutes after power-on
- Reporting interval: 60 seconds (or on significant change: ±0.5°C, ±5 AQI)
- LED colors: Green (AQI 0-10), Yellow→Red (AQI 10-200)
- Pairing: Auto on first boot, button-hold 3s any time

## 🚩 5. TECHNICAL DEBT & INCOMPLETENESS TRACKER

### Dead Code
| Location | Description | Risk |
|---|---|---|
| firmware/main/history.c:353 | DEBUG comment for ring buffer wrap detection | Low - active debug code, not dead |

### Incomplete Features
| Location | Description | Blocker |
|---|---|---|
| firmware/main/environmental.c | Placeholder module for environmental compensation | Awaiting implementation - blocking feature |

### Gaps & Workarounds
| Location | Description | Workaround in Use |
|---|---|---|
| ZHA sensors show "Unknown" | BUG in main repo: quirk expects manf-specific attrs, but PUBLIC firmware also sends non-manuf frames (zigbee.c:355) | Set `is_manufacturer_specific=False` in `zha/aircube.py` — confirmed works in custom_patch quirk |

## ♻️ 6. EVOLUTION & DECOUPLING

| Priority | Task | Rationale |
|----------|------|----------|
| 1 | **CRITICAL: Apply quirk fix to main repo** | Bug: sensors show "Unknown" in ZHA — set `is_manufacturer_specific=False` in `zha/aircube.py` |
| 2 | Add test coverage for history + serial | `tests/test_led_color_lib.py` exists; history/serial untested |
| 3 | Implement `environmental.c` placeholder or remove | Adds confusion, resolve state |
| 4 | Add support for AQI-UBA over Zigbee | Currently serial outputs both, Zigbee sends only AQI-S (see zigbee.c:483) |
| 5 | Fix staged doc claim about missing zigbee.c | Public source DOES have zigbee.c — remove incorrect claim from custom_patch/README.md |
| 6 | Optional: Merge Lovelace dashboard | custom_patch has dashboard card — decide if it belongs in main repo |

---

> **⚠️ PENDING SIGN-OFF:** This knowledge base is a draft. It is not considered authoritative until reviewed and approved by `StuckAtPrototype`.