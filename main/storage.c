/* Persist the cumulative water counter to NVS so we don't lose the total
 * across power cuts or OTA updates. We write throttled (every N minutes
 * or every N liters delta) to spare flash erase cycles. */

#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "wm_storage";
static const char *NS  = "watermeter";
static const char *KEY = "total_lx1k";   /* liters * 1000 */

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

uint64_t storage_load_liters_x1000(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved total - starting from zero");
        return 0;
    }
    uint64_t value = 0;
    esp_err_t err = nvs_get_u64(h, KEY, &value);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no saved total - starting from zero");
        return 0;
    }
    ESP_LOGI(TAG, "loaded total: %llu (L x 1000)", (unsigned long long)value);
    return value;
}

esp_err_t storage_save_liters_x1000(uint64_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u64(h, KEY, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "saved total: %llu", (unsigned long long)value);
    }
    return err;
}
