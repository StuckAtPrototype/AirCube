/**
 * Zigbee2MQTT External Converter for AirCube
 *
 * Place this file in your Zigbee2MQTT data directory and reference it
 * in configuration.yaml:
 *
 *   external_converters:
 *     - aircube.js
 *
 * Standard clusters (auto-handled by Z2M):
 *   - Temperature Measurement (0x0402)
 *   - Relative Humidity (0x0405)
 *
 * Custom cluster 0xFC01 attributes (read-only sensors):
 *   0x0000 = eCO2       (uint16, ppm)
 *   0x0001 = eTVOC      (uint16, ppb)
 *   0x0002 = AQI        (uint16, index)
 *
 * Analog Output cluster 0x000D (writable):
 *   0x0055 = presentValue (float, 0-100 brightness)
 */

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
