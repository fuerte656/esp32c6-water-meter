const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const utils = require('zigbee-herdsman-converters/lib/utils');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const ea = exposes.access;
const e = exposes.presets;

const METER_ENDPOINTS = { meter1: 10, meter2: 11 };
const BATTERY_ENDPOINT = METER_ENDPOINTS.meter1;

function readUint48(raw) {
    if (raw === null || raw === undefined) return 0;
    if (typeof raw === 'bigint') return Number(raw);
    if (typeof raw === 'number') return raw;
    if (Array.isArray(raw)) {
        return (raw[1] || 0) * 0x100000000 + (raw[0] || 0);
    }
    if (typeof raw === 'object') {
        if ('low' in raw && 'high' in raw) {
            return raw.high * 0x100000000 + raw.low;
        }
    }
    return Number(raw) || 0;
}

const fzWaterSummation = {
    cluster: 'seMetering',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (meta && meta.logger) {
            meta.logger.info(`[water_meter] seMetering ep=${msg.endpoint.ID}: ${JSON.stringify(msg.data)}`);
        }
        if (!msg.data || msg.data.currentSummDelivered === undefined) {
            return {};
        }
        const liters = readUint48(msg.data.currentSummDelivered);
        const m3 = Number((liters / 1000).toFixed(3));
        /* postfixWithEndpointName returns a SUFFIXED KEY STRING (e.g.
         * "water_consumed_meter1"), not a payload object. Must be
         * applied per-key. */
        return {
            [utils.postfixWithEndpointName('water_consumed', msg, model, meta)]: m3,
            [utils.postfixWithEndpointName('water_consumed_liters', msg, model, meta)]: liters,
        };
    },
};

const tzWaterRead = {
    key: ['water_consumed', 'water_consumed_liters'],
    convertGet: async (entity, key, meta) => {
        await entity.read('seMetering', ['currentSummDelivered']);
    },
};

function hasBatteryCluster(device) {
    const ep = device && device.getEndpoint(BATTERY_ENDPOINT);
    if (!ep) return false;
    if (typeof ep.supportsInputCluster === 'function') {
        return ep.supportsInputCluster('genPowerCfg');
    }
    /* Fallback for older zigbee-herdsman versions: inspect raw list. */
    const ids = (ep.inputClusters || []).map((c) => (typeof c === 'object' ? c.ID : c));
    return ids.includes(1); /* genPowerCfg = 0x0001 */
}

const baseExposes = [
    exposes.numeric('water_consumed', ea.STATE_GET)
        .withEndpoint('meter1')
        .withUnit('m³')
        .withDescription('Total water consumed (meter 1)'),
    exposes.numeric('water_consumed_liters', ea.STATE_GET)
        .withEndpoint('meter1')
        .withUnit('L')
        .withDescription('Total water consumed in liters (meter 1)'),
    exposes.numeric('water_consumed', ea.STATE_GET)
        .withEndpoint('meter2')
        .withUnit('m³')
        .withDescription('Total water consumed (meter 2)'),
    exposes.numeric('water_consumed_liters', ea.STATE_GET)
        .withEndpoint('meter2')
        .withUnit('L')
        .withDescription('Total water consumed in liters (meter 2)'),
];

const definition = {
    fingerprint: [
        { modelID: 'ESP32C6.WaterMeter', manufacturerName: 'DIY' },
    ],
    zigbeeModel: ['ESP32C6.WaterMeter'],
    model: 'ESP32C6_WATER',
    vendor: 'DIY',
    description: 'ESP32-C6 Zigbee impulse water meter (dual)',
    fromZigbee: [fzWaterSummation, fz.battery],
    toZigbee: [tzWaterRead],
    /* Exposes is a function so we only advertise battery entities when
     * the firmware was built with WM_BATTERY_MONITORING=1 and the
     * genPowerCfg cluster is actually present on the device. */
    exposes: (device, options) => {
        if (device && hasBatteryCluster(device)) {
            return [...baseExposes, e.battery(), e.battery_voltage()];
        }
        return baseExposes;
    },
    endpoint: (device) => METER_ENDPOINTS,
    meta: { multiEndpoint: true },
    configure: async (device, coordinatorEndpoint, logger) => {
        for (const epId of Object.values(METER_ENDPOINTS)) {
            const endpoint = device.getEndpoint(epId);
            await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering']);
            await endpoint.configureReporting('seMetering', [{
                attribute: 'currentSummDelivered',
                minimumReportInterval: 10,
                maximumReportInterval: 3600,
                reportableChange: 1,
            }]);
        }

        /* Battery cluster lives on the first endpoint only, and only
         * when the firmware is built with WM_BATTERY_MONITORING=1.
         * Inspect the device's interview data to skip the bind +
         * configureReporting calls entirely when the cluster isn't
         * present - that avoids the 10 s timeout per call. */
        if (hasBatteryCluster(device)) {
            const battEp = device.getEndpoint(BATTERY_ENDPOINT);
            await reporting.bind(battEp, coordinatorEndpoint, ['genPowerCfg']);
            await reporting.batteryPercentageRemaining(battEp);
            await reporting.batteryVoltage(battEp);
        } else if (logger && logger.info) {
            logger.info('[water_meter] genPowerCfg cluster not advertised by device; skipping battery reporting (firmware built without WM_BATTERY_MONITORING).');
        }
    },
};

module.exports = definition;
