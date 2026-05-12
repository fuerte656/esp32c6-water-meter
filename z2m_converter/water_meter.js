const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const ea = exposes.access;

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
            meta.logger.info(`[water_meter] seMetering report: ${JSON.stringify(msg.data)}`);
        }
        const result = {};
        if (msg.data && msg.data.currentSummDelivered !== undefined) {
            const liters = readUint48(msg.data.currentSummDelivered);
            result.water_consumed = Number((liters / 1000).toFixed(3));
            result.water_consumed_liters = liters;
        }
        return result;
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
    description: 'ESP32-C6 Zigbee impulse water meter',
    fromZigbee: [fzWaterSummation],
    toZigbee: [tzWaterRead],
    exposes: [
        exposes.numeric('water_consumed', ea.STATE_GET)
            .withUnit('m³')
            .withDescription('Total water consumed'),
        exposes.numeric('water_consumed_liters', ea.STATE_GET)
            .withUnit('L')
            .withDescription('Total water consumed in liters'),
    ],
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint = device.getEndpoint(10);
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering']);
        await endpoint.configureReporting('seMetering', [{
            attribute: 'currentSummDelivered',
            minimumReportInterval: 10,
            maximumReportInterval: 3600,
            reportableChange: 1,
        }]);
    },
    meta: {},
};

module.exports = definition;