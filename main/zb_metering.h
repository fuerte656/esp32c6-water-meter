#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Start the Zigbee task and bring up the network. Registers one
 * endpoint per meter (WM_NUM_METERS endpoints total). */
esp_err_t zb_metering_start(void);

/* Push the current cumulative total (in liters * 1000) for meter idx
 * to its Metering cluster. Sends a manual report if the device is
 * joined. */
void zb_metering_update_total(int idx, uint64_t liters_x1000);

/* Push a battery percentage (0-100) to the PowerCfg cluster on the
 * first endpoint. No-op if WM_BATTERY_MONITORING is 0. */
void zb_metering_update_battery(uint8_t percent);
