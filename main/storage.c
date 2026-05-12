/* Persist the cumulative water counters to NVS so we don't lose the
 * totals across power cuts or OTA updates. We write throttled (every
 * N minutes or every N liters delta) to spare flash erase cycles.
 * One key per meter. */

#include "storage.h"
#include "wm_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "wm_storage";
static const char *NS  = "watermeter";

static void key_for(int idx, char out[16])
{
    /* Keep "total_lx1k" as meter 0's key so existing single-meter
     * deployments upgrade cleanly. */
    if (idx == 0) {
        snprintf(out, 16, "total_lx1k");
    } else {
        snprintf(out, 16, "total_lx1k_%d", idx + 1);
    }
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, doing it now");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

uint64_t storage_load_liters_x1000(int idx)
{
    if (idx < 0 || idx >= WM_NUM_METERS) return 0;

    char key[16];
    key_for(idx, key);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "meter[%d] no saved total - starting from zero", idx);
        return 0;
    }
    uint64_t value = 0;
    esp_err_t err = nvs_get_u64(h, key, &value);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "meter[%d] no saved total - starting from zero", idx);
        return 0;
    }
    ESP_LOGI(TAG, "meter[%d] loaded total: %llu (L x 1000)",
             idx, (unsigned long long)value);
    return value;
}

esp_err_t storage_save_liters_x1000(int idx, uint64_t value)
{
    if (idx < 0 || idx >= WM_NUM_METERS) return ESP_ERR_INVALID_ARG;

    char key[16];
    key_for(idx, key);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u64(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "meter[%d] saved total: %llu",
                 idx, (unsigned long long)value);
    }
    return err;
}
