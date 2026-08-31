/* Water leak probe.
 *
 * A leak probe is a slow, bistable input - unlike the reed switches
 * there is no pulse to catch, just a level that flips when water
 * bridges the contacts. So there is no PCNT unit and no interrupt
 * here: the level is sampled with a multi-sample debounce, either
 * once per second by the always-on supervisor or once per wake in
 * deep-sleep mode.
 *
 * The debounced state lives in RTC slow memory so a deep-sleep wake
 * can tell "still wet" (nothing to report beyond the heartbeat) from
 * "just got wet" (send a status change notification immediately).
 */

#include "leak_sensor.h"
#include "wm_config.h"

#if WM_LEAK_SENSOR

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wm_leak";

/* Number of agreeing samples required across WM_LEAK_DEBOUNCE_MS. */
#define LEAK_SAMPLES  8

RTC_DATA_ATTR static bool s_leak_active = false;

esp_err_t leak_sensor_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = BIT64(WM_LEAK_GPIO),
        .mode         = GPIO_MODE_INPUT,
        /* Bias the pin to the inactive level so an open probe reads
         * "dry": pull-up for an active-low probe, pull-down for an
         * active-high one. While the probe is wet the pull resistor
         * sinks ~70 uA; that only happens during an actual leak. */
        .pull_up_en   = WM_LEAK_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE
                                             : GPIO_PULLUP_ENABLE,
        .pull_down_en = WM_LEAK_ACTIVE_LEVEL ? GPIO_PULLDOWN_ENABLE
                                             : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    s_leak_active = leak_sensor_sample();

    ESP_LOGI(TAG, "init on GPIO %d, active %s, debounce %d ms, state: %s",
             WM_LEAK_GPIO, WM_LEAK_ACTIVE_LEVEL ? "high" : "low",
             WM_LEAK_DEBOUNCE_MS, s_leak_active ? "WET" : "dry");
    return ESP_OK;
}

bool leak_sensor_sample(void)
{
    const TickType_t step = pdMS_TO_TICKS(WM_LEAK_DEBOUNCE_MS / LEAK_SAMPLES);
    bool active = true;

    for (int i = 0; i < LEAK_SAMPLES; i++) {
        if (gpio_get_level(WM_LEAK_GPIO) != WM_LEAK_ACTIVE_LEVEL) {
            active = false;
            break;
        }
        if (i + 1 < LEAK_SAMPLES) {
            vTaskDelay(step ? step : 1);
        }
    }
    return active;
}

bool leak_sensor_state(void)
{
    return s_leak_active;
}

bool leak_sensor_poll(bool *out_state)
{
    bool now = leak_sensor_sample();
    bool changed = (now != s_leak_active);
    s_leak_active = now;
    if (out_state) *out_state = now;
    if (changed) {
        ESP_LOGW(TAG, "leak state changed: %s", now ? "WET" : "dry");
    }
    return changed;
}

uint64_t leak_sensor_wake_mask(void)
{
#if WM_LEAK_WAKES_FROM_SLEEP
    /* Arming a pin that already sits at the wake level would wake the
     * chip immediately and spin the battery flat. While wet we rely on
     * the WM_KEEPALIVE_PERIOD_S timer wake to notice it drying out. */
    return s_leak_active ? 0 : BIT64(WM_LEAK_GPIO);
#else
    return 0;
#endif
}

#else  /* !WM_LEAK_SENSOR */

esp_err_t leak_sensor_init(void)          { return ESP_OK; }
bool      leak_sensor_sample(void)        { return false; }
bool      leak_sensor_state(void)         { return false; }
bool      leak_sensor_poll(bool *out)     { if (out) *out = false; return false; }
uint64_t  leak_sensor_wake_mask(void)     { return 0; }

#endif /* WM_LEAK_SENSOR */
