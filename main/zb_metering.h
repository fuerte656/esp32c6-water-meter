#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Start the Zigbee task and bring up the network. Registers one
 * endpoint per meter (WM_NUM_METERS endpoints total), plus one IAS
 * Zone endpoint for the leak probe when WM_LEAK_SENSOR is 1. */
esp_err_t zb_metering_start(void);

/* Push the current cumulative total (in liters * 1000) for meter idx
 * to its Metering cluster. Sends a manual report if the device is
 * joined. */
void zb_metering_update_total(int idx, uint64_t liters_x1000);

/* Push battery telemetry to the PowerCfg cluster on the first
 * endpoint. percent is 0..100; mv is the measured battery voltage in
 * millivolts (0 if unknown). No-op if WM_BATTERY_MONITORING is 0. */
void zb_metering_update_battery(uint8_t percent, uint32_t mv);

/* Push the water leak state to the IAS Zone cluster on the leak
 * endpoint: sets ZoneStatus bit 0 (Alarm1) and sends a Zone Status
 * Change Notification when the device is joined. No-op if
 * WM_LEAK_SENSOR is 0. */
void zb_metering_update_leak(bool leak);

/* True once the device has joined / rejoined a Zigbee network. */
bool zb_metering_is_joined(void);

/* Block until joined or timeout_ms elapses. Returns true if joined. */
bool zb_metering_wait_joined(uint32_t timeout_ms);
