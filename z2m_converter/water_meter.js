const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const utils = require('zigbee-herdsman-converters/lib/utils');
const ea = exposes.access;

const METER_ENDPOINTS = { meter1: 10, meter2: 11 };

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
        const payload = {
            water_consumed: Number((liters / 1000).toFixed(3)),
            water_consumed_liters: liters,
        };
        return utils.postfixWithEndpointName(payload, msg, model, meta);
    },
};

const tzWaterRead = {
    key: ['water_consumed', 'water_consumed_liters'],
    convertGet: async (entity, key, meta) => {
        await entity.read('seMetering', ['currentSummDelivered']);
    },
};

const definition = {
    fingerprint: [
        { modelID: 'esp32c6', manufacturerName: 'ESPRESSIF' },
    ],
    zigbeeModel: ['esp32c6', 'ESP32C6.WaterMeter'],
    model: 'ESP32C6_WATER',
    vendor: 'DIY',
    description: 'ESP32-C6 Zigbee impulse water meter (dual)',
    fromZigbee: [fzWaterSummation],
    toZigbee: [tzWaterRead],
    exposes: [
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
    ],
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
    },
};

module.exports = definition;
