#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Water leak probe on WM_LEAK_GPIO. All functions are no-ops (and
 * report "dry") when WM_LEAK_SENSOR is 0, so callers don't need to
 * guard every call site. */

/* Configure the probe GPIO and seed the cached state from a first
 * debounced sample. */
esp_err_t leak_sensor_init(void);

/* Debounced instantaneous read: true = water detected. Blocks for
 * WM_LEAK_DEBOUNCE_MS. */
bool leak_sensor_sample(void);

/* Last known state. Kept in RTC memory so it survives deep sleep and
 * a wake can tell "still wet" from "just got wet". */
bool leak_sensor_state(void);

/* Take a fresh debounced sample, store it, and return true if the
 * state changed since the last call. The new state is written to
 * *out_state when out_state is non-NULL. */
bool leak_sensor_poll(bool *out_state);

/* Deep-sleep wake mask contribution for the probe. Returns 0 when the
 * probe can't be armed - either because it's active high (the wake
 * mask is wake-on-low, see WM_LEAK_WAKES_FROM_SLEEP) or because it's
 * already wet, in which case arming it would wake the chip again the
 * instant it sleeps. */
uint64_t leak_sensor_wake_mask(void);
