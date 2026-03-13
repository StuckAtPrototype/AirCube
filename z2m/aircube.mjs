/**
 * Zigbee2MQTT External Converter for AirCube (Z2M 2.x)
 *
 * Z2M 2.x requires ES module format (.mjs). For Z2M 1.x, use aircube.js instead.
 *
 * Place this file in your Zigbee2MQTT external_converters directory and
 * reference it in configuration.yaml:
 *
 *   external_converters:
 *     - external_converters/aircube.mjs
 *
 * Standard clusters (auto-handled by Z2M):
 *   - Temperature Measurement (0x0402)
 *   - Relative Humidity (0x0405)
 *
 * Custom cluster 0xFC01 attributes:
 *   0x0000 = eCO2  (uint16, ppm)
 *   0x0001 = eTVOC (uint16, ppb)
 *   0x0002 = AQI   (uint16, index)
 */

import {temperature, humidity} from 'zigbee-herdsman-converters/lib/modernExtend';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const e = exposes.presets;

// Z2M 2.x requires the cluster ID as a string for custom (non-standard) clusters.
const CUSTOM_CLUSTER_ID = '64513'; // 0xFC01

const ATTR_ECO2  = 0x0000;
const ATTR_ETVOC = 0x0001;
const ATTR_AQI   = 0x0002;

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

const definition = {
    zigbeeModel: ['AirCube'],
    model: 'AirCube',
    vendor: 'StuckAtPrototype',
    description: 'AirCube air quality monitor',
    extend: [
        temperature(),
        humidity(),
    ],
    fromZigbee: [fzAirCubeAirQuality],
    toZigbee: [],
    exposes: [
        e.numeric('eco2', exposes.access.STATE)
            .withUnit('ppm')
            .withDescription('Equivalent carbon dioxide concentration')
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
    ],
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(10);
        /* Bind standard clusters */
        await endpoint.bind('msTemperatureMeasurement', coordinatorEndpoint);
        await endpoint.bind('msRelativeHumidity', coordinatorEndpoint);
        /* Configure reporting for standard clusters */
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

export default definition;
