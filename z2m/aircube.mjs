/**
 * Zigbee2MQTT External Converter for AirCube (Z2M 2.x)
 *
 * Z2M 2.x requires ES module format (.mjs). For Z2M 1.x, use aircube.js instead.
 *
 * Installation (Z2M 2.x): drop this file into the `external_converters` folder
 * next to your Zigbee2MQTT `configuration.yaml` and restart Zigbee2MQTT.
 * Do NOT add an `external_converters:` entry to configuration.yaml -- that
 * setting was removed in Z2M 2.0 and leaving it in place stops this converter
 * (and sometimes Z2M itself) from starting.
 *
 * Exposed values:
 *   Custom cluster 0xFC01 (matches zha/aircube.py)
 *     0x0000 = eco2  (uint16, ppm)   -> Equivalent CO2
 *     0x0001 = etvoc (uint16, ppb)   -> VOC parts
 *     0x0002 = aqi   (uint16, 0-500) -> VOC level (TVOC-derived)
 *   Analog Output 0x000D  -> Brightness (writable, 0-100%)
 *   Temperature 0x0402, Humidity 0x0405
 *   Pro only: CO2 0x040D (SCD41), Illuminance 0x0400 (VCNL4040)
 */

import {Zcl} from 'zigbee-herdsman';
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

const AIR_QUALITY_CLUSTER = 'aircubeAirQuality';

const hasInputCluster = (device, cluster) =>
    Boolean(device?.endpoints?.some((endpoint) => endpoint.supportsInputCluster(cluster)));

/**
 * Base units do not implement the SCD41/VCNL4040 clusters at all, so their
 * exposes are resolved per device instead of statically.
 */
const whenClusterPresent = (cluster, extend) => ({
    ...extend,
    exposes: [
        (device, options) => {
            if (!hasInputCluster(device, cluster)) return [];
            return (extend.exposes ?? []).flatMap((expose) =>
                typeof expose === 'function' ? expose(device, options) : [expose],
            );
        },
    ],
});

const definition = {
    zigbeeModel: ['AirCube'],
    model: 'AirCube',
    vendor: 'StuckAtPrototype',
    description: 'AirCube air quality monitor',
    extend: [
        m.deviceAddCustomCluster(AIR_QUALITY_CLUSTER, {
            name: AIR_QUALITY_CLUSTER,
            ID: 0xfc01,
            attributes: {
                eco2: {name: 'eco2', ID: 0x0000, type: Zcl.DataType.UINT16},
                etvoc: {name: 'etvoc', ID: 0x0001, type: Zcl.DataType.UINT16},
                aqi: {name: 'aqi', ID: 0x0002, type: Zcl.DataType.UINT16},
            },
            commands: {},
            commandsResponse: {},
        }),
        m.temperature({reporting: {min: '10_SECONDS', max: '1_MINUTE', change: 50}}),
        m.humidity({reporting: {min: '10_SECONDS', max: '1_MINUTE', change: 100}}),
        m.numeric({
            name: 'eco2',
            label: 'Equivalent CO2',
            cluster: AIR_QUALITY_CLUSTER,
            attribute: 'eco2',
            description: 'Equivalent CO2 estimated from VOC (ENS16x)',
            unit: 'ppm',
            access: 'STATE_GET',
            valueMin: 400,
            valueMax: 8192,
            reporting: {min: '10_SECONDS', max: '1_MINUTE', change: 50},
        }),
        m.numeric({
            name: 'voc',
            label: 'VOC parts',
            cluster: AIR_QUALITY_CLUSTER,
            attribute: 'etvoc',
            description: 'Equivalent total VOC (tVOC)',
            unit: 'ppb',
            access: 'STATE_GET',
            valueMin: 0,
            valueMax: 65535,
            reporting: {min: '10_SECONDS', max: '1_MINUTE', change: 10},
        }),
        m.numeric({
            name: 'aqi',
            label: 'VOC level',
            cluster: AIR_QUALITY_CLUSTER,
            attribute: 'aqi',
            description: 'TVOC-derived VOC level (0-500)',
            access: 'STATE_GET',
            valueMin: 0,
            valueMax: 500,
            reporting: {min: '10_SECONDS', max: '1_MINUTE', change: 5},
        }),
        m.numeric({
            name: 'brightness',
            label: 'Brightness',
            cluster: 'genAnalogOutput',
            attribute: 'presentValue',
            description: 'LED brightness',
            unit: '%',
            access: 'ALL',
            valueMin: 0,
            valueMax: 100,
            valueStep: 1,
            precision: 0,
            reporting: {min: '10_SECONDS', max: '1_MINUTE', change: 5},
        }),
        whenClusterPresent('msCO2', m.co2()),
        whenClusterPresent('msIlluminanceMeasurement', m.illuminance()),
    ],
};

export default definition;
