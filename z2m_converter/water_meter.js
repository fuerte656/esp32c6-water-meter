const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const utils = require('zigbee-herdsman-converters/lib/utils');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const ea = exposes.access;
const e = exposes.presets;

const METER_ENDPOINTS = { meter1: 10, meter2: 11 };
const LEAK_ENDPOINT = 12;
const ENDPOINTS = { ...METER_ENDPOINTS, leak: LEAK_ENDPOINT };
const BATTERY_ENDPOINT = METER_ENDPOINTS.meter1;
/* Must match WM_LEAK_ZONE_ID in main/wm_config.h. */
const LEAK_ZONE_ID = 0x2a;

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

/* The firmware publishes leak state as an IAS Zone Status Change
 * Notification, but a plain zoneStatus read/report is also accepted so
 * the state can be recovered after a Z2M restart. Bit 0 = Alarm1. */
const fzWaterLeak = {
    cluster: 'ssIasZone',
    type: ['commandStatusChangeNotification', 'attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const raw = msg.type === 'commandStatusChangeNotification'
            ? msg.data.zonestatus
            : msg.data.zoneStatus;
        if (raw === undefined || raw === null) return {};
        return {
            water_leak: (raw & 1) > 0,
            tamper: (raw & (1 << 2)) > 0,
            battery_low: (raw & (1 << 3)) > 0,
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

function hasLeakCluster(device) {
    const ep = device && device.getEndpoint(LEAK_ENDPOINT);
    if (!ep) return false;
    if (typeof ep.supportsInputCluster === 'function') {
        return ep.supportsInputCluster('ssIasZone');
    }
    const ids = (ep.inputClusters || []).map((c) => (typeof c === 'object' ? c.ID : c));
    return ids.includes(0x500); /* ssIasZone */
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

/* The leak entities are NOT endpoint-suffixed: the firmware only has
 * one probe, so plain `water_leak` is what HA's moisture device class
 * expects. */
const leakExposes = [e.water_leak(), e.tamper(), e.battery_low()];

const definition = {
    fingerprint: [
        { modelID: 'ESP32C6.WaterMeter', manufacturerName: 'DIY' },
    ],
    zigbeeModel: ['ESP32C6.WaterMeter'],
    model: 'ESP32C6_WATER',
    vendor: 'DIY',
    description: 'ESP32-C6 Zigbee impulse water meter (dual) with leak probe',
    fromZigbee: [fzWaterSummation, fzWaterLeak, fz.battery],
    toZigbee: [tzWaterRead],
    /* Exposes is a function so we only advertise entities the firmware
     * actually built: battery needs WM_BATTERY_MONITORING=1, leak needs
     * WM_LEAK_SENSOR=1. Both are detected from the clusters the device
     * advertised during the interview. */
    exposes: (device, options) => {
        let list = baseExposes;
        if (device && hasLeakCluster(device)) {
            list = [...list, ...leakExposes];
        }
        if (device && hasBatteryCluster(device)) {
            list = [...list, e.battery(), e.battery_voltage()];
        }
        return list;
    },
    endpoint: (device) => ENDPOINTS,
    meta: { multiEndpoint: true },
    configure: async (device, coordinatorEndpoint, logger) => {
        /* Force the displayed power source and device type. Z2M caches
         * device.type from the initial association and never refreshes
         * it, so a device that originally joined while the firmware was
         * misconfigured as ED will be stuck as EndDevice forever. We
         * pick the source-of-truth from the genPowerCfg cluster:
         * present  => battery build (WM_BATTERY_MONITORING=1)
         * absent   => USB/mains build. */
        const battery = hasBatteryCluster(device);
        const desiredPower = battery ? 'Battery' : 'Mains (single phase)';
        const desiredType  = battery ? 'EndDevice' : 'Router';
        let changed = false;
        if (device.powerSource !== desiredPower) {
            device.powerSource = desiredPower;
            changed = true;
        }
        if (device.type !== desiredType) {
            device.type = desiredType;
            changed = true;
        }
        if (changed) {
            device.save();
            if (logger && logger.info) {
                logger.info(`[water_meter] forced powerSource=${desiredPower}, type=${desiredType} (battery cluster=${battery})`);
            }
        }

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

        /* IAS Zone enrollment for the leak endpoint. The device only
         * sends Zone Status Change Notifications to its CIE, so the
         * coordinator IEEE has to be written first; the firmware's
         * stack answers with a Zone Enroll Request, which we ack. The
         * bind is what lets the notification reach Z2M. */
        if (hasLeakCluster(device)) {
            const leakEp = device.getEndpoint(LEAK_ENDPOINT);
            await reporting.bind(leakEp, coordinatorEndpoint, ['ssIasZone']);
            await leakEp.write('ssIasZone', {
                iasCieAddr: coordinatorEndpoint.deviceIeeeAddress,
            });
            await leakEp.command('ssIasZone', 'enrollRsp', {
                enrollrspcode: 0,
                zoneid: LEAK_ZONE_ID,
            }, { disableDefaultResponse: true });
            /* Seed the initial state; the device answers with the
             * current zoneStatus, which fzWaterLeak turns into
             * water_leak. */
            await leakEp.read('ssIasZone', ['zoneState', 'zoneStatus']);
            if (logger && logger.info) {
                logger.info(`[water_meter] IAS Zone enrolled on endpoint ${LEAK_ENDPOINT} (zone id ${LEAK_ZONE_ID})`);
            }
        } else if (logger && logger.info) {
            logger.info('[water_meter] ssIasZone cluster not advertised by device; skipping leak sensor setup (firmware built without WM_LEAK_SENSOR).');
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
