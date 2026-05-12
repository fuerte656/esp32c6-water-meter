/*
 * Zigbee2MQTT external converter for the ESP32-C6 DIY water meter.
 *
 * Place in the Z2M data directory and reference from configuration.yaml:
 *   external_converters:
 *     - water_meter.js
 *
 * The firmware uses the seMetering cluster but does NOT expose the
 * Multiplier/Divisor attributes (esp-zigbee-lib has no public helper to
 * add them to the metering cluster). The default modernExtend handler
 * for seMetering insists on reading them and throws UNSUPPORTED_ATTRIBUTE,
 * so we replace it entirely with a minimal handler that just decodes
 * CurrentSummationDelivered and exposes water totals.
 */

const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const ea = exposes.access;

function readUint48(raw) {
    if (typeof raw === 'bigint') return Number(raw);
    if (typeof raw === 'number') return raw;
    if (raw && typeof raw === 'object' && 'low' in raw && 'high' in raw) {
        return raw.high * 0x100000000 + raw.low;
    }
    return Number(raw);
}

const fzWaterSummation = {
    cluster: 'seMetering',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data && msg.data.currentSummDelivered !== undefined) {
            const liters = readUint48(msg.data.currentSummDelivered);
            return {
                water_consumed: Number((liters / 1000).toFixed(3)),
                water_consumed_liters: liters,
            };
        }
    },
};

const definition = {
    /* Match by both modelID AND manufacturer for an unambiguous hit. */
    fingerprint: [
        { modelID: 'ESP32C6.WaterMeter', manufacturerName: 'DIY' },
    ],
    /* Also keep zigbeeModel as a fallback for older matchers. */
    zigbeeModel: ['ESP32C6.WaterMeter'],
    model: 'ESP32C6_WATER',
    vendor: 'DIY',
    description: 'ESP32-C6 Zigbee impulse water meter',
    fromZigbee: [fzWaterSummation],
    toZigbee: [],
    exposes: [
        exposes.numeric('water_consumed', ea.STATE)
            .withUnit('m³')
            .withDescription('Total water consumed'),
        exposes.numeric('water_consumed_liters', ea.STATE)
            .withUnit('L')
            .withDescription('Total water consumed in liters'),
    ],
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint = device.getEndpoint(10);
        await reporting.bind(endpoint, coordinatorEndpoint, ['seMetering']);
        // Reporting is already configured device-side via
        // esp_zb_zcl_update_reporting_info() in the firmware.
    },
    meta: {},
};

module.exports = definition;