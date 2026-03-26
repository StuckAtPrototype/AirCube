# Connecting AirCube to Home Assistant

This guide walks you through adding your AirCube air quality monitor to Home Assistant over Zigbee. After setup, you'll have live temperature, humidity, eCO2, eTVOC, and AQI readings plus a brightness slider in your smart home dashboard.

The AirCube works with both **ZHA** (built-in) and **Zigbee2MQTT**. Pick whichever you already use. If you're starting fresh, ZHA is simpler.

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

Use this method if you're using Home Assistant's built-in **Zigbee Home Automation** integration (the default). No extra add-ons required.

## A1 -- Set Up ZHA

If you already have ZHA running with your coordinator, skip to A2.

1. Plug your Zigbee coordinator dongle into your Home Assistant machine.
2. Go to **Settings > Devices & Services > Add Integration**.
3. Search for **Zigbee Home Automation (ZHA)** and add it.
4. Select your coordinator from the serial port list and follow the prompts.

## A2 -- Add the AirCube Quirk

The AirCube uses a custom Zigbee cluster (0xFC01) for air quality data and a standard Analog Output cluster (0x000D) for LED brightness. The quirk below tells ZHA to create **sensor entities** for eCO2, eTVOC, and AQI, plus a **brightness slider** (0--100%).

1. Install the **File editor** add-on if you don't have it:
   - **Settings > Add-ons > Add-on Store** -- search **File editor**, install, start it.

2. Open **File editor** from the sidebar.

3. Create a folder called **`custom_zha_quirks`** in your `/config/` directory (the same folder that contains your `configuration.yaml`).

4. Inside `custom_zha_quirks`, create a new file called **`aircube.py`** and paste this content:

```python
"""StuckAtPrototype AirCube air quality monitor quirk for ZHA."""

from zigpy.quirks import CustomCluster
from zigpy.quirks.v2 import QuirkBuilder
from zigpy.quirks.v2.homeassistant import EntityType
from zigpy.zcl.foundation import ZCLAttributeDef
import zigpy.types as t

try:
    from zigpy.quirks.v2.homeassistant.sensor import SensorDeviceClass, SensorStateClass
except ImportError:
    from homeassistant.components.sensor import SensorDeviceClass, SensorStateClass


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
        fallback_name="Volatile organic compounds",
    )
    .sensor(
        AirQualityCluster.AttributeDefs.aqi.name,
        AirQualityCluster.cluster_id,
        endpoint_id=10,
        device_class=SensorDeviceClass.AQI,
        state_class=SensorStateClass.MEASUREMENT,
        fallback_name="Air quality index",
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
```

5. Open your main **`configuration.yaml`** (in `/config/`) and add:

   ```yaml
   zha:
     custom_quirks_path: /config/custom_zha_quirks/
     enable_quirks: true
   ```

   > **Important:** The trailing `/` on the path is required in Home Assistant 2026.x and works fine on older versions too.

   If you already have a `zha:` section, just add the two lines underneath it.

6. **Restart Home Assistant** from **Settings > System > Restart**.
7. **Remove and re-pair** the AirCube once after adding the quirk (ZHA caches device data at first join).

## A3 -- Pair the AirCube

1. Go to **Settings > Devices & Services > ZHA**.
2. Click **Add Device**.
3. **Plug in your AirCube** via USB-C. On first power-up, it automatically enters pairing mode.

   > **Already plugged in?** Hold the button on the AirCube for **3 seconds**. The LEDs will start flashing blue.

4. Wait 10-30 seconds. The AirCube will appear in ZHA. Give it a name like `AirCube Living Room`.

5. When the LEDs stop flashing blue and return to a steady color, pairing is complete.

## A4 -- Verify Sensors

Go to **Settings > Devices & Services > ZHA** and click on the AirCube device. You should see five sensors and a brightness control:

| Entity | What It Does | Unit |
|--------|-------------|------|
| Temperature | Room temperature | C |
| Humidity | Relative humidity | % |
| Equivalent CO2 | eCO2 concentration (estimated) | ppm |
| Volatile organic compounds | eTVOC concentration | ppb |
| Air quality index | Overall air quality | -- |
| Brightness | LED brightness (slider) | 0--100 |

> Temperature and humidity are detected automatically by ZHA. eCO2, eTVOC, and AQI come from the custom quirk. The brightness slider uses the standard Analog Output cluster.

---

# Method B -- Zigbee2MQTT

Use this method if you prefer Zigbee2MQTT or already have it running.

## B1 -- Install MQTT Broker

1. Go to **Settings > Add-ons > Add-on Store**.
2. Search for **Mosquitto broker**, click **Install**, then **Start**.
3. Go to **Settings > Devices & Services > Add Integration**.
4. Search for **MQTT** and add it. Accept the defaults.

## B2 -- Install Zigbee2MQTT

1. Go to **Settings > Add-ons > Add-on Store**.
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

1. Go to **Settings > Add-ons > Zigbee2MQTT > Configuration** tab.
2. Set the serial port:
   ```yaml
   serial:
     port: /dev/ttyACM0
   ```
3. Enable **Start on boot** and **Watchdog**, then click **Start**.

## B5 -- Add the AirCube Converter

1. Open **File editor** (install from Add-on Store if needed).
2. Navigate to the `zigbee2mqtt` folder.
3. Create a new file called **`aircube.js`** and paste:

```javascript
const {temperature, humidity} = require('zigbee-herdsman-converters/lib/modernExtend');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;

const CUSTOM_CLUSTER_ID = 0xFC01;
const ATTR_ECO2  = 0x0000;
const ATTR_ETVOC = 0x0001;
const ATTR_AQI   = 0x0002;

const ANALOG_OUTPUT_CLUSTER = 'genAnalogOutput';
const ATTR_PRESENT_VALUE = 0x0055;

const fzAirCubeAirQuality = {
    cluster: CUSTOM_CLUSTER_ID,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const result = {};
        if (msg.data.hasOwnProperty(ATTR_ECO2)) {
            result.eco2 = msg.data[ATTR_ECO2];
        }
        if (msg.data.hasOwnProperty(ATTR_ETVOC)) {
            result.voc = msg.data[ATTR_ETVOC];
        }
        if (msg.data.hasOwnProperty(ATTR_AQI)) {
            result.aqi = msg.data[ATTR_AQI];
        }
        return result;
    },
};

const fzAirCubeBrightness = {
    cluster: ANALOG_OUTPUT_CLUSTER,
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data.hasOwnProperty('presentValue')) {
            return {brightness: Math.round(msg.data.presentValue)};
        }
    },
};

const tzAirCubeBrightness = {
    key: ['brightness'],
    convertSet: async (entity, key, value, meta) => {
        await entity.write(ANALOG_OUTPUT_CLUSTER, {presentValue: value});
        return {state: {brightness: value}};
    },
    convertGet: async (entity, key, meta) => {
        await entity.read(ANALOG_OUTPUT_CLUSTER, ['presentValue']);
    },
};

const definition = {
    zigbeeModel: ['AirCube'],
    model: 'AirCube',
    vendor: 'StuckAtPrototype',
    description: 'AirCube air quality monitor',
    extend: [
        temperature(),
        humidity(),
    ],
    fromZigbee: [fzAirCubeAirQuality, fzAirCubeBrightness],
    toZigbee: [tzAirCubeBrightness],
    exposes: [
        e.numeric('eco2', exposes.access.STATE)
            .withUnit('ppm')
            .withDescription('Equivalent CO2 concentration')
            .withValueMin(400)
            .withValueMax(8192),
        e.numeric('voc', exposes.access.STATE)
            .withUnit('ppb')
            .withDescription('Total volatile organic compounds')
            .withValueMin(0)
            .withValueMax(65535),
        e.numeric('aqi', exposes.access.STATE)
            .withUnit('')
            .withDescription('Air Quality Index')
            .withValueMin(0)
            .withValueMax(500),
        e.numeric('brightness', exposes.access.ALL)
            .withDescription('LED brightness')
            .withValueMin(0)
            .withValueMax(100),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(10);
        await endpoint.bind('msTemperatureMeasurement', coordinatorEndpoint);
        await endpoint.bind('msRelativeHumidity', coordinatorEndpoint);
        await endpoint.configureReporting('msTemperatureMeasurement', [{
            attribute: 'measuredValue', minimumReportInterval: 1,
            maximumReportInterval: 60, reportableChange: 50,
        }]);
        await endpoint.configureReporting('msRelativeHumidity', [{
            attribute: 'measuredValue', minimumReportInterval: 1,
            maximumReportInterval: 60, reportableChange: 100,
        }]);
    },
};

module.exports = definition;
```

4. Open **`configuration.yaml`** in the `zigbee2mqtt` folder and add:

   ```yaml
   external_converters:
     - aircube.js
   ```

5. **Restart Zigbee2MQTT** from the add-on page.

## B6 -- Pair the AirCube

1. In the Zigbee2MQTT dashboard, click **Permit join (All)**.
2. **Plug in your AirCube** via USB-C (or hold the button 3 seconds if already plugged in).
3. Wait for the LEDs to stop flashing blue.
4. Name the device in Zigbee2MQTT (e.g. `AirCube Living Room`).

## B7 -- Verify Sensors

Go to **Settings > Devices & Services > MQTT** and click on the AirCube. You should see five sensors (Temperature, Humidity, eCO2, eTVOC, AQI) plus a Brightness control.

---

# Dashboard

These cards work with both ZHA and Zigbee2MQTT.

### Quick Entities Card

Edit your dashboard, click **Add Card**, choose **Entities**, and select:
- AirCube Temperature
- AirCube Humidity
- AirCube Equivalent CO2
- AirCube Volatile organic compounds
- AirCube Air quality index
- AirCube Brightness

### AQI Gauge

Add a **Manual card** and paste:

```yaml
type: gauge
entity: sensor.aircube_living_room_air_quality_index
name: Air Quality
min: 0
max: 200
severity:
  green: 0
  yellow: 50
  red: 100
```

### 24-Hour History

```yaml
type: history-graph
title: Air Quality - Last 24 Hours
hours_to_show: 24
entities:
  - entity: sensor.aircube_living_room_temperature
  - entity: sensor.aircube_living_room_humidity
  - entity: sensor.aircube_living_room_air_quality_index
```

> Entity names depend on what you named the device. Check **Settings > Devices & Services** for the exact entity IDs.

---

## LED Reference

| LED Behavior | Meaning |
|-------------|---------|
| Steady green | Good air quality (AQI 0-10) |
| Yellow to red gradient | Degrading air quality (AQI 10-200) |
| Flashing blue | Pairing mode (searching for Zigbee network) |
| Off | Brightness set to 0 (press button to cycle) |

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

### Temperature and humidity show up but eCO2 / eTVOC / AQI are missing

- The custom quirk (ZHA) or converter (Z2M) is not loaded.
- **ZHA:** Check that `custom_quirks_path` is set in `configuration.yaml` and the `aircube.py` file is in the right folder. Restart Home Assistant, then remove and re-pair the AirCube.
- **Firmware:** Make sure you are running the latest AirCube firmware from this repo. It actively sends attribute reports for the custom cluster so ZHA updates the sensors.
- **Z2M:** Check that `external_converters` is in the Z2M `configuration.yaml` and `aircube.js` is in the `zigbee2mqtt` folder. Restart Zigbee2MQTT.

### eCO2 / eTVOC / AQI values are stuck at 0

This is normal for the first 5 minutes after power-on. The air quality sensor needs to warm up. Once ready, values will start updating (typically within 60 seconds).

### I want to pair the AirCube to a different Home Assistant

Hold the button for 3 seconds to re-enter pairing mode. If the device won't leave its old network, unplug it, plug it back in, and immediately hold the button for 3 seconds while it boots.

### Sensor values only update every 10 seconds

This is by design. The AirCube pushes new sensor values over Zigbee every 10 seconds. Additionally, the ZCL reporting configuration will send an immediate update when a reading changes significantly (temperature by 0.5 C, eCO2 by 50 ppm, AQI by 5 points, etc.).

### Can I use multiple AirCubes?

Yes. The quirk/converter applies to every AirCube automatically. Just pair each one and give it a unique name. Each gets its own set of sensors and brightness control.
