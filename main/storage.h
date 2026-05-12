#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t storage_init(void);

/* Load saved liters x 1000 from NVS. Returns 0 on first boot. */
uint64_t storage_load_liters_x1000(void);

/* Save liters x 1000 to NVS. */
esp_err_t storage_save_liters_x1000(uint64_t value);
