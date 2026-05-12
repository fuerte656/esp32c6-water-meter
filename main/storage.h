#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t storage_init(void);

/* Load saved liters x 1000 from NVS for the given meter index.
 * Returns 0 on first boot. */
uint64_t storage_load_liters_x1000(int idx);

/* Save liters x 1000 to NVS for the given meter index. */
esp_err_t storage_save_liters_x1000(int idx, uint64_t value);
