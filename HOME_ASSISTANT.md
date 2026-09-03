# Connecting AirCube to Home Assistant

This guide walks you through adding your AirCube air quality monitor to Home Assistant over Zigbee. After setup, you'll have live temperature, humidity, eCO2, eTVOC, and VOC Level readings plus a brightness slider in your smart home dashboard.

The AirCube works with both **ZHA** (built-in) and **Zigbee2MQTT**. Pick whichever you already use. If you're starting fresh, ZHA is simpler.

**Prefer to follow along?** [Watch the AirCube ZHA integration walkthrough on YouTube](https://www.youtube.com/watch?v=rpkR3O64rY8).

> **A note on names.** Home Assistant renamed a couple of things in 2026.2, so the menus below are listed with both names -- use whichever your version shows:
>
> | HA 2026.2 and newer | HA 2026.1 and earlier |
> |---------------------|-----------------------|
> | **Apps** / **App Store** | **Add-ons** / **Add-on Store** |
> | **Settings > Zigbee** | **Settings > Devices & Services > ZHA** |
>
> Nothing functional changed -- only the labels. The integration is still called **Zigbee Home Automation (ZHA)** in the Add Integration list.

---

## What You Need

- **AirCube** -- powered via USB-C
- **Zigbee coordinator dongle** -- plugs into your Home Assistant machine
- **Home Assistant** -- running on any supported hardware (Raspberry Pi, mini PC, etc.)

### Recommended Zigbee Coordinators

Any Zigbee 3.0 coordinator works. If you don't have one yet, the **SONOFF ZBDongle-E** is the easiest to get started with (~$13).

| Dongle | Notes |
|--------|-------|
| SONOFF ZBDongle-E | Best value, widely available |
| SONOFF ZBDongle-P | Proven, large community |
| ConBee II / III | Also works, popular alternative |

---

# Method A -- ZHA (Recommended)

Use this method if you're using Home Assistant's built-in **Zigbee Home Automation** integration (the default). No extra apps (add-ons) required.

## A1 -- Set Up ZHA

If you already have ZHA running with your coordinator, skip to A2.

1. Plug your Zigbee coordinator dongle into your Home Assistant machine.
2. Go to **Settings > Devices & Services > Add Integration**.
3. Search for **Zigbee Home Automation (ZHA)** and add it.
4. Select your coordinator from the serial port list and follow the prompts.

## A2 -- Add the AirCube Quirk

The AirCube uses a custom Zigbee cluster (0xFC01) for air quality data and a standard Analog Output cluster (0x000D) for LED brightness. The quirk below tells ZHA to create **sensor entities** for eCO2, eTVOC, and VOC Level, plus a **brightness slider** (0--100%).

1. Install the **File editor** app (add-on) if you don't have it:
   - **Settings > Apps > App Store** (**Settings > Add-ons > Add-on Store** on HA 2026.1 and earlier) -- search **File editor**, install, start it.

2. Open **File editor** from the sidebar.

3. Create a folder called **`custom_zha_quirks`** **next to your `configuration.yaml`**.

   > **Where do I create it?** Open the File editor and look for `configuration.yaml`. On **HA 2026.x** it's in `/homeassistant/`, on **HA 2025.x and earlier** it's in `/config/`. Create `custom_zha_quirks` in whichever folder contains your `configuration.yaml`. **Do not** create a new folder called `config` -- just put `custom_zha_quirks` directly alongside `configuration.yaml`.

4. Inside `custom_zha_quirks`, create a new file called **`aircube.py`** and paste this content.
   This is kept in sync with [`zha/aircube.py`](zha/aircube.py) in this repo -- if you'd rather not
   copy/paste, you can grab the file directly from there instead.

```python
"""StuckAtPrototype AirCube air quality monitor quirk for ZHA.

This single file is compatible with both old and new Home Assistant:

  * Modern HA (zigpy >= 0.65.1, i.e. HA >= ~2024.8):
        Full quirks v2 support. Exposes eCO2, tVOC and AQI sensors from the
        custom 0xFC01 cluster plus a Brightness number on the Analog Output
        cluster.

  * Old HA (zigpy < 0.65, e.g. HA 2024.1.x):
        `zigpy.quirks.v2` does not exist yet, so importing QuirkBuilder raises
        ImportError and the whole quirk fails to load. We fall back to a classic
        v1 quirk so the module imports cleanly and the device is named correctly.

        IMPORTANT: on these old ZHA versions there is no supported way to turn
        custom-cluster (0xFC01) attributes into entities -- eCO2/tVOC/AQI will
        NOT appear until HA is updated to a version with quirks v2, or the values
        are read over BLE via the firmware's BTHome broadcaster. Temperature,
        humidity and brightness still work on old HA because they use standard
        Zigbee clusters and are discovered automatically.
"""

from zigpy.quirks import CustomCluster
from zigpy.zcl.foundation import ZCLAttributeDef
import zigpy.types as t


class AirQualityCluster(CustomCluster):
    """AirCube custom air quality cluster (0xFC01) — read-only sensors."""

    cluster_id = 0xFC01
    name = "AirCube Air Quality"
    ep_attribute = "aircube_air_quality"

    class AttributeDefs(CustomCluster.AttributeDefs):
        eco2 = ZCLAttributeDef(
            id=0x0000, type=t.uint16_t, is_manufacturer_specific=False
        )
        etvoc = ZCLAttributeDef(
            id=0x0001, type=t.uint16_t, is_manufacturer_specific=False
        )
        aqi = ZCLAttributeDef(
            id=0x0002, type=t.uint16_t, is_manufacturer_specific=False
        )


ANALOG_OUTPUT_CLUSTER_ID = 0x000D


# ---------------------------------------------------------------------------
# Detect whether this HA/zigpy version supports the quirks v2 API.
# ---------------------------------------------------------------------------
try:
    from zigpy.quirks.v2 import QuirkBuilder
    from zigpy.quirks.v2.homeassistant import EntityType

    try:
        from zigpy.quirks.v2.homeassistant.sensor import (
            SensorDeviceClass,
            SensorStateClass,
        )
    except ImportError:
        from homeassistant.components.sensor import (
            SensorDeviceClass,
            SensorStateClass,
        )

    _HAS_QUIRKS_V2 = True
except ImportError:
    _HAS_QUIRKS_V2 = False


if _HAS_QUIRKS_V2:
    # -----------------------------------------------------------------------
    # Modern HA: full quirks v2 definition.
    # -----------------------------------------------------------------------
    (
        QuirkBuilder("StuckAtPrototype", "AirCube")
        .replaces(AirQualityCluster, endpoint_id=10)
        .sensor(
            AirQualityCluster.AttributeDefs.eco2.name,
            AirQualityCluster.cluster_id,
            endpoint_id=10,
            unit="ppm",
            translation_key="equivalent_co2",
            state_class=SensorStateClass.MEASUREMENT,
            fallback_name="Equivalent CO2",
        )
        .sensor(
            AirQualityCluster.AttributeDefs.etvoc.name,
            AirQualityCluster.cluster_id,
            endpoint_id=10,
            unit="ppb",
            device_class=SensorDeviceClass.VOLATILE_ORGANIC_COMPOUNDS_PARTS,
            state_class=SensorStateClass.MEASUREMENT,
            fallback_name="tVOC",
        )
        .sensor(
            AirQualityCluster.AttributeDefs.aqi.name,
            AirQualityCluster.cluster_id,
            endpoint_id=10,
            state_class=SensorStateClass.MEASUREMENT,
            translation_key="voc_level",
            fallback_name="VOC Level",
        )
        .number(
            "present_value",
            ANALOG_OUTPUT_CLUSTER_ID,
            endpoint_id=10,
            min_value=0,
            max_value=100,
            step=1,
            mode="slider",
            entity_type=EntityType.STANDARD,
            translation_key="brightness",
            fallback_name="Brightness",
        )
        .add_to_registry()
    )
else:
    # -----------------------------------------------------------------------
    # Old HA (no quirks v2): classic v1 fallback.
    #
    # This keeps the module importable (stops the ImportError crash) and names
    # the device. Temperature, humidity and brightness are exposed by ZHA's
    # standard discovery. The eCO2/tVOC/AQI values on cluster 0xFC01 cannot be
    # surfaced as entities on this ZHA version -- update HA, or read them over
    # BLE (BTHome), for those.
    # -----------------------------------------------------------------------
    from zigpy.quirks import CustomDevice
    from zigpy.profiles import zha
    from zigpy.zcl.clusters.general import AnalogOutput, Basic, Identify
    from zigpy.zcl.clusters.measurement import (
        RelativeHumidity,
        TemperatureMeasurement,
    )
    from zhaquirks.const import (
        DEVICE_TYPE,
        ENDPOINTS,
        INPUT_CLUSTERS,
        MODELS_INFO,
        OUTPUT_CLUSTERS,
        PROFILE_ID,
    )

    class AirCube(CustomDevice):
        """AirCube v1 quirk for HA versions without quirks v2."""

        signature = {
            MODELS_INFO: [("StuckAtPrototype", "AirCube")],
            ENDPOINTS: {
                # <SimpleDescriptor endpoint=10 profile=260 device_type=770
                #  input_clusters=[0, 3, 13, 1026, 1029, 64513]
                #  output_clusters=[]>
                10: {
                    PROFILE_ID: zha.PROFILE_ID,
                    DEVICE_TYPE: zha.DeviceType.TEMPERATURE_SENSOR,
                    INPUT_CLUSTERS: [
                        Basic.cluster_id,
                        Identify.cluster_id,
                        AnalogOutput.cluster_id,
                        TemperatureMeasurement.cluster_id,
                        RelativeHumidity.cluster_id,
                        AirQualityCluster.cluster_id,
                    ],
                    OUTPUT_CLUSTERS: [],
                },
            },
        }

        replacement = {
            ENDPOINTS: {
                10: {
                    PROFILE_ID: zha.PROFILE_ID,
                    DEVICE_TYPE: zha.DeviceType.TEMPERATURE_SENSOR,
                    INPUT_CLUSTERS: [
                        Basic,
                        Identify,
                        AnalogOutput,
                        TemperatureMeasurement,
                        RelativeHumidity,
                        AirQualityCluster,
                    ],
                    OUTPUT_CLUSTERS: [],
                },
            },
        }
```

5. Open your main **`configuration.yaml`** (in `/config/`) and add:

   ```yaml
   zha:
     custom_quirks_path: config/custom_zha_quirks/
     enable_quirks: true
   ```

   > **Note:** Use `config/custom_zha_quirks/` exactly as shown -- this path works on **both** HA 2025.x and 2026.x, even though the File editor in 2026.x shows the root as `/homeassistant/`. The trailing `/` is required on HA 2026.x. **Do not** change `/config/` to `/homeassistant/` in this setting.

   If you already have a `zha:` section, just add the two lines underneath it.

6. **Restart Home Assistant** from **Settings > System > Restart**.
7. **Remove and re-pair** the AirCube once after adding the quirk (ZHA caches device data at first join).

## A3 -- Pair the AirCube

1. Go to **Settings > Zigbee** (**Settings > Devices & Services > ZHA** on HA 2026.1 and earlier).
2. Click **Add Device**.
3. **Plug in your AirCube** via USB-C. It boots into BLE mode by default -- Zigbee pairing is not automatic.
4. **Hold the button on the AirCube for 3 seconds.** The LEDs will start flashing blue, and the device reboots into Zigbee mode to begin network steering.
5. Wait 10-30 seconds. The AirCube will appear in ZHA. Give it a name like `AirCube Living Room`.
6. When the LEDs stop flashing blue and return to a steady color, pairing is complete.

## A4 -- Verify Sensors

Go to **Settings > Zigbee > Devices** (**Settings > Devices & Services > ZHA** on HA 2026.1 and earlier) and click on the AirCube device. You should see six entities:

| Entity | What It Does | Unit |
|--------|-------------|------|
| Temperature | Room temperature | C |
| Humidity | Relative humidity | % |
| Equivalent CO2 | eCO2 concentration (estimated) | ppm |
| tVOC | eTVOC concentration | ppb |
| VOC Level | TVOC-derived VOC Level (0--500) | -- |
| Brightness | LED brightness (slider) | 0--100 |

> Temperature and humidity are detected automatically by ZHA. eCO2, eTVOC, and VOC Level come from the custom quirk. The brightness slider uses the standard Analog Output cluster.
>
> **AirCube Pro:** The current ZHA quirk exposes these same six entities. The Pro's dedicated true CO2 and illuminance sensors are not yet exposed by ZHA; use Zigbee2MQTT 2.x if you need those two entities in Home Assistant.

---

# Method B -- Zigbee2MQTT

Use this method if you prefer Zigbee2MQTT or already have it running.

## B1 -- Install MQTT Broker

1. Go to **Settings > Apps > App Store** (**Settings > Add-ons > Add-on Store** on HA 2026.1 and earlier).
2. Search for **Mosquitto broker**, click **Install**, then **Start**.
3. Go to **Settings > Devices & Services > Add Integration**.
4. Search for **MQTT** and add it. Accept the defaults.

## B2 -- Install Zigbee2MQTT

1. Go to **Settings > Apps > App Store** (**Settings > Add-ons > Add-on Store** on HA 2026.1 and earlier).
2. Click the **three-dot menu** (top-right) > **Repositories**.
3. Add this URL:
   ```
   https://github.com/zigbee2mqtt/hassio-zigbee2mqtt
   ```
4. Search for **Zigbee2MQTT** and click **Install**.

## B3 -- Plug In Your Coordinator

1. Plug the Zigbee dongle into your Home Assistant machine.
2. Go to **Settings > System > Hardware** > three-dot menu > **All Hardware**.
3. Find your dongle. Write down its path (e.g. `/dev/ttyACM0`).

## B4 -- Configure and Start Zigbee2MQTT

1. Go to **Settings > Apps > Zigbee2MQTT > Configuration** tab (**Settings > Add-ons > ...** on HA 2026.1 and earlier).
2. Set the serial port:
   ```yaml
   serial:
     port: /dev/ttyACM0
   ```
3. Enable **Start on boot** and **Watchdog**, then click **Start**.

## B5 -- Add the AirCube Converter

The converter file format depends on your Zigbee2MQTT version:
- **Z2M 2.x** (2024+): Uses ES modules (`.mjs`)
- **Z2M 1.x** (legacy): Uses CommonJS (`.js`)

Both converter files are in the [`z2m/`](z2m/) folder of this repo.

### Z2M 2.x (Recommended)

1. Open **File editor** (install from the **App Store** / **Add-on Store** if needed).
2. Navigate to the `zigbee2mqtt` folder (the one containing `configuration.yaml`) and create an `external_converters` subfolder next to it.
3. Copy [`z2m/aircube.mjs`](z2m/aircube.mjs) into the `external_converters` folder.
4. Open **`configuration.yaml`** in the `zigbee2mqtt` folder and **enable external JavaScript**:

   ```yaml
   advanced:
     enable_external_js: true
   ```

   If you already have an `advanced:` section, just add the one line under it. **This step is required.** Zigbee2MQTT ships with external converters switched off, and when they are off Z2M never even looks in the `external_converters` folder -- you get no error, just a device with no custom sensors. There is an equivalent **Enable external JS** toggle in the Z2M web UI under **Settings > Advanced**.

5. In the same **`configuration.yaml`**, **remove** any `external_converters:` block if one is present:

   ```yaml
   # Delete these lines -- they break converter loading on Z2M 2.x
   external_converters:
     - external_converters/aircube.mjs
   ```

   Z2M 2.0 removed this setting. Everything inside the `external_converters` folder is now loaded automatically, and leaving the old setting in place stops the converter (and sometimes Z2M itself) from starting.

6. **Restart Zigbee2MQTT** from its app (add-on) page.
7. Check the Zigbee2MQTT log. You should see `Loaded external converter 'aircube.mjs'.` If you instead see `External JS (converters/extensions) is disabled`, step 4 did not take effect.

### Sample `configuration.yaml`

A complete, working Z2M 2.x config with the AirCube converter enabled. Yours will have different values for `network_key`, `port`, and your MQTT credentials -- copy the structure, not the secrets.

```yaml
mqtt:
  base_topic: zigbee2mqtt
  server: mqtt://core-mosquitto:1883
  user: your_mqtt_user
  password: your_mqtt_password

serial:
  port: /dev/ttyACM0

advanced:
  # Keep whatever network_key you already have. Changing it un-pairs every device.
  network_key: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
  # Required for the AirCube converter. Off by default; nothing in
  # external_converters/ is read while this is false.
  enable_external_js: true

frontend:
  enabled: true
  port: 8099

homeassistant:
  enabled: true

devices:
  '0x1051dbfffe65fc77':
    friendly_name: AirCube Living Room
```

The converter itself is **not** referenced anywhere in this file. `aircube.mjs` just has to exist in the `external_converters` folder next to `configuration.yaml`.

Two mistakes that are easy to make here:

- **Do not add a second `advanced:` block.** If `enable_external_js: false` is already in your config, edit that line in place. YAML does not merge duplicate keys -- Z2M parses the file with `js-yaml`, which treats a repeated top-level key as a fatal error, so the bridge will refuse to start.
- **Do not add an `external_converters:` list.** That setting was removed in Z2M 2.0. Leaving it in place breaks converter loading.

### Z2M 1.x (Legacy)

1. Open **File editor**.
2. Copy [`z2m/aircube.js`](z2m/aircube.js) into the `zigbee2mqtt` folder.
3. Open **`configuration.yaml`** in the `zigbee2mqtt` folder and add:

   ```yaml
   external_converters:
     - aircube.js
   ```

4. **Restart Zigbee2MQTT** from its app (add-on) page.

## B6 -- Pair the AirCube

1. In the Zigbee2MQTT dashboard, click **Permit join (All)**.
2. **Plug in your AirCube** via USB-C (or hold the button 3 seconds if already plugged in).
3. Wait for the LEDs to stop flashing blue.
4. Name the device in Zigbee2MQTT (e.g. `AirCube Living Room`).

## B7 -- Verify Sensors

Go to **Settings > Devices & Services > MQTT** and click on the AirCube. You should see six entities:

| Entity | What It Does | Unit |
|--------|-------------|------|
| Temperature | Room temperature | C |
| Humidity | Relative humidity | % |
| Equivalent CO2 | eCO2 concentration (estimated) | ppm |
| VOC parts | eTVOC concentration | ppb |
| VOC level | TVOC-derived VOC Level (0--500) | -- |
| Brightness | LED brightness (slider) | 0--100 |

On the **Pro**, two more entities appear from the dedicated sensors when using **Zigbee2MQTT 2.x with `aircube.mjs`**: **CO2** (true CO2 from the SCD41, in ppm) and **Illuminance** (ambient light from the VCNL4040, in lx). These are absent on the Base. The legacy Zigbee2MQTT 1.x `aircube.js` converter exposes only the six core entities.

> If the device card says *"Automatically generated definition"*, the converter did **not** load. See the troubleshooting section below.

---

# Dashboard

These cards work with both ZHA and Zigbee2MQTT.

### Quick Entities Card

Edit your dashboard, click **Add Card**, choose **Entities**, and select:
- AirCube Temperature
- AirCube Humidity
- AirCube Equivalent CO2
- AirCube tVOC
- AirCube VOC Level
- AirCube Brightness

### VOC Level Gauge

Add a **Manual card** and paste:

```yaml
type: gauge
entity: sensor.aircube_living_room_voc_level
name: Air Quality
min: 0
max: 500
severity:
  green: 0
  yellow: 50
  red: 200
```

### 24-Hour History

```yaml
type: history-graph
title: Air Quality - Last 24 Hours
hours_to_show: 24
entities:
  - entity: sensor.aircube_living_room_temperature
  - entity: sensor.aircube_living_room_humidity
  - entity: sensor.aircube_living_room_voc_level
```

> Entity names depend on what you named the device. Check **Settings > Devices & Services** for the exact entity IDs.

---

## LED Reference

The LED follows **canonical VOC Level** (TVOC-derived) on a continuous green-to-red gradient. The hue moves linearly with VOC Level, so the color fades smoothly rather than stepping between bands. eCO2 does **not** affect the LED.

| LED color | VOC Level | TVOC (ppb) | Rating |
|-----------|-----|------------|--------|
| Steady green | 0--10 | 0--~43 | Excellent |
| Green → lime | 10--50 | ~43--220 | Good |
| Lime → yellow | 50--100 | 220--650 | Moderate |
| Yellow → orange → red | 100--200 | 650--2,200 | Poor |
| Steady red | 200+ | 2,200+ | Unhealthy |
| Flashing blue | -- | -- | Pairing mode (searching for Zigbee network) |
| Off | -- | -- | Brightness set to 0 (press button to cycle) |

> On firmware **1.4.3 and below**, the same gradient was driven by **AQI-S** (relative) instead of canonical VOC Level. See the [README LED Reference](README.md#led-reference) for the full mapping.

### Button

| Action | Result |
|--------|--------|
| Short press | Cycle LED brightness (off, 10%, 30%, 60%, 100%) |
| Hold 3 seconds | Enter Zigbee pairing mode (LEDs flash blue) |

---

## Troubleshooting

### The AirCube LEDs flash blue but it never connects

- Make sure pairing/permit join is enabled in ZHA or Zigbee2MQTT.
- Move the AirCube closer to the coordinator. Zigbee works best within 10-30 meters indoors.
- Check that your coordinator is online in the integration dashboard.

### Temperature and humidity show up but eCO2 / eTVOC / VOC Level are missing

- The custom quirk (ZHA) or converter (Z2M) is not loaded.
- **ZHA:** Check that `custom_quirks_path` is set in `configuration.yaml` and the `aircube.py` file is in the right folder. The path in `configuration.yaml` must be `/config/custom_zha_quirks/` (not `/homeassistant/...`). Restart Home Assistant, then remove and re-pair the AirCube.
- **ZHA (HA 2026.x):** The File editor shows the root as `/homeassistant/` instead of `/config/`. **Do not** create a new folder called `config` inside `/homeassistant/`. Place `custom_zha_quirks` directly inside `/homeassistant/`, next to `configuration.yaml`. The path in `configuration.yaml` should still say `/config/custom_zha_quirks/`.
- **Firmware:** Make sure you are running the latest AirCube firmware from this repo. It actively sends attribute reports for the custom cluster so ZHA updates the sensors.
- **Firmware version:** The device reports its build as the Zigbee Basic cluster **Software build ID** (`sw_build_id`, attribute `0x4000` on cluster `0x0000`, endpoint `10`). In ZHA you can read it under the device’s **Manage Zigbee device** UI. The string comes from ESP-IDF’s app version (`firmware/version.txt` at build time).
- **Z2M 2.x:** Open the device page in Home Assistant. If it says **"Automatically generated definition"**, Z2M never loaded `aircube.mjs` and is guessing from the raw Zigbee clusters — which is why only the standard ones (temperature, humidity, CO2, illuminance, "Analog output 10") show up. The Z2M log tells you which failure it is; check, in order:
  - The log line **`External JS (converters/extensions) is disabled`** at startup. This is the most common cause. Set `advanced: enable_external_js: true` in `configuration.yaml` (see the [sample config](#sample-configurationyaml)) and restart. While it is disabled Z2M does not read the `external_converters` folder at all, so there is no error message about the converter — only a `Device ... is NOT supported` warning.
  - `aircube.mjs` sits in an `external_converters` folder **next to** `configuration.yaml` (on the HA app/add-on that is `/homeassistant/zigbee2mqtt/external_converters/`, **not** `.../data/external_converters/`). If the folder or filename is wrong, Z2M skips it silently.
  - There is **no** `external_converters:` block left in `configuration.yaml`. That setting was removed in Z2M 2.0 and its presence prevents loading.
  - You're using `aircube.mjs`, not `aircube.js` — Z2M 2.x requires ES module format. If Z2M renames the file to `aircube.mjs.invalid`, the converter has a load error; check the Z2M logs.
  - After fixing any of the above, restart Z2M and then **re-interview** the device (device page > *Reconfigure*) so the custom cluster is registered.
- **Z2M 1.x:** Check that `external_converters` is in the Z2M `configuration.yaml` and `aircube.js` is in the `zigbee2mqtt` folder. Restart Zigbee2MQTT.

### eCO2 / eTVOC / VOC Level values are stuck at 0

This is normal for the first 5 minutes after power-on. The air quality sensor needs to warm up. Once ready, values will start updating (typically within 60 seconds).

### I want to pair the AirCube to a different Home Assistant

Hold the button for 3 seconds to re-enter pairing mode. If the device won't leave its old network, unplug it, plug it back in, and immediately hold the button for 3 seconds while it boots.

### Sensor values only update every 10 seconds

This is by design. The AirCube pushes new sensor values over Zigbee every 10 seconds. Additionally, the ZCL reporting configuration will send an immediate update when a reading changes significantly (temperature by 0.5 C, eCO2 by 50 ppm, VOC Level by 5 points, etc.).

### Can I use multiple AirCubes?

Yes. The quirk/converter applies to every AirCube automatically. Just pair each one and give it a unique name. Each gets its own set of sensors and brightness control.
