#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Start the Zigbee task and bring up the network. */
esp_err_t zb_metering_start(void);

/* Push the current cumulative total (in liters * 1000) to the
 * Metering cluster. Sends a manual report if the device is joined. */
void zb_metering_update_total(uint64_t liters_x1000);

/* Push a battery percentage (0-100) to the PowerCfg cluster. No-op
 * if WM_BATTERY_MONITORING is 0. */
void zb_metering_update_battery(uint8_t percent);
