/* ESP32-C6 Zigbee Dual Water Meter
 *
 * Counts pulses from up to WM_NUM_METERS reed/hall-switch water
 * meters, persists each total to NVS, and reports them over Zigbee
 * using the Smart Energy Metering cluster (one endpoint per meter).
 * Operates as Router on USB power, or End Device (optionally with
 * deep sleep) on batteries.
 *
 * See wm_config.h for the WM_POWER_USB / WM_DEEP_SLEEP flags and the
 * per-meter GPIO assignments.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_sleep.h"

#include "wm_config.h"
#include "pulse_counter.h"
#include "storage.h"
#include "zb_metering.h"

#if WM_BATTERY_MONITORING
#include "battery.h"
#endif

static const char *TAG = "wm_main";

/* =====================================================================
 * USB / always-on supervisor task
 * Runs in both WM_POWER_USB=1 mode and WM_POWER_USB=0,WM_DEEP_SLEEP=0
 * mode. Polls the counters every second, pushes changes to Zigbee,
 * and persists to NVS at the configured cadence.
 * =====================================================================
 */
#if !WM_USE_DEEP_SLEEP

static void supervisor_task(void *arg)
{
    uint64_t last_pushed[WM_NUM_METERS];
    uint64_t last_saved[WM_NUM_METERS];
    int64_t  last_save_us[WM_NUM_METERS];

    int64_t now_us0 = esp_timer_get_time();
    for (int i = 0; i < WM_NUM_METERS; i++) {
        last_pushed[i]  = UINT64_MAX;
        last_saved[i]   = pulse_counter_get_total_liters_x1000(i);
        last_save_us[i] = now_us0;
    }

#if WM_BATTERY_MONITORING
    int64_t  last_batt_us  = 0;
#endif

    while (1) {
        int64_t now_us = esp_timer_get_time();

        for (int i = 0; i < WM_NUM_METERS; i++) {
            int new_pulses = pulse_counter_take(i);
            uint64_t total_x1000 = pulse_counter_get_total_liters_x1000(i);

            if (total_x1000 != last_pushed[i]) {
                if (new_pulses > 0) {
                    ESP_LOGI(TAG, "meter[%d] +%d pulses (total=%llu L x1000)",
                             i, new_pulses, (unsigned long long)total_x1000);
                }
                zb_metering_update_total(i, total_x1000);
                last_pushed[i] = total_x1000;
            }

            bool time_due  = (now_us - last_save_us[i]) >=
                             (int64_t)WM_NVS_FLUSH_PERIOD_S * 1000000LL;
            bool delta_due = (total_x1000 > last_saved[i]) &&
                             (total_x1000 - last_saved[i]) >=
                             (uint64_t)WM_NVS_FLUSH_DELTA_LITERS * 1000ULL;

            if (total_x1000 != last_saved[i] && (time_due || delta_due)) {
                storage_save_liters_x1000(i, total_x1000);
                ESP_LOGI(TAG, "meter[%d] persisted total: %llu L (x1000)",
                         i, (unsigned long long)total_x1000);
                last_saved[i]   = total_x1000;
                last_save_us[i] = now_us;
            }
        }

#if WM_BATTERY_MONITORING
        /* Sample the battery roughly every minute. */
        if ((now_us - last_batt_us) >= 60LL * 1000000LL) {
            uint8_t pct = battery_read_percent();
            zb_metering_update_battery(pct);
            last_batt_us = now_us;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif /* !WM_USE_DEEP_SLEEP */

/* =====================================================================
 * Deep-sleep wake handler (WM_USE_DEEP_SLEEP=1 only)
 * Each wake runs app_main from scratch. We figure out why we woke,
 * count any pulses on whichever GPIO is low, send Zigbee reports for
 * every meter, and go back to sleep.
 * =====================================================================
 */
#if WM_USE_DEEP_SLEEP

#include "driver/gpio.h"

static void deep_sleep_cycle(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        /* Software-debounce by sampling each pulse line: any that's
         * still low (closed) is a real pulse. With two meters it's
         * possible (though unlikely) for both to fire in the same
         * wake-up window - we count each one individually. */
        for (int i = 0; i < WM_NUM_METERS; i++) {
            int gpio = pulse_counter_gpio(i);
            gpio_set_direction(gpio, GPIO_MODE_INPUT);
            gpio_pullup_en(gpio);
        }
        vTaskDelay(pdMS_TO_TICKS(WM_SW_DEBOUNCE_MS));

        for (int i = 0; i < WM_NUM_METERS; i++) {
            int gpio = pulse_counter_gpio(i);
            if (gpio_get_level(gpio) == 0) {
                pulse_counter_increment_one(i);
                ESP_LOGI(TAG, "Wake from GPIO: meter[%d] +1 pulse", i);
            }
        }
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Wake from timer (keepalive)");
    } else {
        ESP_LOGI(TAG, "Wake cause: %d (cold boot or reset)", cause);
    }

    /* Push every meter's value to Zigbee and persist to NVS. */
    uint64_t wake_mask = 0;
    for (int i = 0; i < WM_NUM_METERS; i++) {
        uint64_t total_x1000 = pulse_counter_get_total_liters_x1000(i);
        ESP_LOGI(TAG, "meter[%d] total now: %llu L x1000",
                 i, (unsigned long long)total_x1000);
        zb_metering_update_total(i, total_x1000);
        storage_save_liters_x1000(i, total_x1000);

        wake_mask |= BIT64(pulse_counter_gpio(i));
    }

#if WM_BATTERY_MONITORING
    uint8_t pct = battery_read_percent();
    zb_metering_update_battery(pct);
#endif

    /* Give Zigbee ~3 seconds to actually send the reports. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Configure all meter GPIOs + the keepalive timer as wake sources
     * and sleep. */
    esp_deep_sleep_enable_gpio_wakeup(wake_mask,
                                      ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_sleep_enable_timer_wakeup((uint64_t)WM_KEEPALIVE_PERIOD_S * 1000000ULL);

    ESP_LOGI(TAG, "Entering deep sleep for up to %d s", WM_KEEPALIVE_PERIOD_S);
    esp_deep_sleep_start();
}

#endif /* WM_USE_DEEP_SLEEP */

/* =====================================================================
 * Entry point
 * =====================================================================
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 Zigbee water meter starting (%d meters)",
             WM_NUM_METERS);
    ESP_LOGI(TAG, "Mode: %s, %s%s",
             WM_POWER_USB ? "USB/Router" : "Battery/EndDevice",
             WM_USE_DEEP_SLEEP ? "deep-sleep" : "always-on",
             WM_BATTERY_MONITORING ? ", battery-mon" : "");

    ESP_ERROR_CHECK(storage_init());

    ESP_ERROR_CHECK(pulse_counter_init());
    for (int i = 0; i < WM_NUM_METERS; i++) {
        uint64_t saved = storage_load_liters_x1000(i);
        pulse_counter_set_initial_liters_x1000(i, saved);
    }

#if WM_BATTERY_MONITORING
    ESP_ERROR_CHECK(battery_init());
#endif

    ESP_ERROR_CHECK(zb_metering_start());

#if WM_USE_DEEP_SLEEP
    /* Give Zigbee a moment to come up and rejoin parent. */
    vTaskDelay(pdMS_TO_TICKS(500));
    deep_sleep_cycle();   /* never returns - sleeps the chip */
#else
    xTaskCreate(supervisor_task, "wm_sup", 4096, NULL, 4, NULL);
#endif
}
