#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Initialise ADC for the battery voltage divider on
 * WM_BATTERY_ADC_GPIO. */
esp_err_t battery_init(void);

/* Read battery percent (0 = empty, 100 = full).
 * For 3xAA cells this maps approximately:
 *   >=4.2 V (3 fresh cells) -> 100 %
 *    3.0 V (3 cells at 1.0 V) -> 0 %
 */
uint8_t battery_read_percent(void);

/* Read raw battery voltage in millivolts. */
uint32_t battery_read_mv(void);
