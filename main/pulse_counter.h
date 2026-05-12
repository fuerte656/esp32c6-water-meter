#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Initialize the PCNT-based pulse counters and software debouncers
 * for all WM_NUM_METERS meters. */
esp_err_t pulse_counter_init(void);

/* Restores a previously persisted total (in liters x 1000) for the
 * given meter index (0 .. WM_NUM_METERS-1). */
void pulse_counter_set_initial_liters_x1000(int idx, uint64_t value);

/* Atomically read & clear the hardware count for meter idx; returns
 * pulses since the last call. Used by the always-on supervisor loop. */
int pulse_counter_take(int idx);

/* Current absolute liters * 1000 for meter idx (initial + accumulated
 * since boot). */
uint64_t pulse_counter_get_total_liters_x1000(int idx);

/* Add one pulse manually to meter idx. Used in deep-sleep mode where
 * the chip is woken by GPIO and PCNT was off during sleep. */
void pulse_counter_increment_one(int idx);

/* GPIO number for meter idx (helper for deep-sleep wake handling). */
int pulse_counter_gpio(int idx);
