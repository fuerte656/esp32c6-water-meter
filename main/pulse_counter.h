#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Initialize the PCNT-based pulse counter and software debouncer. */
esp_err_t pulse_counter_init(void);

/* Restores a previously persisted total (in liters x 1000). */
void pulse_counter_set_initial_liters_x1000(uint64_t value);

/* Atomically read & clear the hardware count; returns pulses since
 * the last call. Used by the always-on supervisor loop. */
int pulse_counter_take(void);

/* Current absolute liters * 1000 (initial + accumulated since boot). */
uint64_t pulse_counter_get_total_liters_x1000(void);

/* Add one pulse manually. Used in deep-sleep mode where the chip is
 * woken by the GPIO and PCNT was off during sleep. */
void pulse_counter_increment_one(void);
