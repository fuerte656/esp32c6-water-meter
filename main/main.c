/* ESP32-C6 Zigbee Dual Water Meter
 *
 * Counts pulses from up to WM_NUM_METERS reed/hall-switch water
 * meters, persists each total to NVS, and reports them over Zigbee
 * using the Smart Energy Metering cluster (one endpoint per meter).
 * With WM_LEAK_SENSOR on, a water leak probe is sampled on
 * WM_LEAK_GPIO and published on its own IAS Zone endpoint.
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
#include "driver/gpio.h"
#include <stdbool.h>

#include "wm_config.h"
#include "pulse_counter.h"
#include "storage.h"
#include "zb_metering.h"
#include "leak_sensor.h"

#if WM_BATTERY_MONITORING
#include "battery.h"
#endif

static const char *TAG = "wm_main";

/* Power up the on-module RF switch and route it to the selected
 * antenna. Must run before zb_metering_start() so the radio comes
 * up with a real RF path. */
static void rf_switch_enable(bool external)
{
    gpio_config_t io = {
        .pin_bit_mask = BIT64(WM_ANTENNA_VDD_GPIO) |
                        BIT64(WM_ANTENNA_CTRL_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* VDD enable is active low. */
    gpio_set_level(WM_ANTENNA_VDD_GPIO, 0);
    gpio_set_level(WM_ANTENNA_CTRL_GPIO, external ? 1 : 0);

    /* Let the switch settle before the radio keys up. */
    vTaskDelay(pdMS_TO_TICKS(1));

    ESP_LOGI(TAG, "RF switch on, antenna: %s",
             external ? "external" : "internal");
}

#if WM_USE_DEEP_SLEEP
/* Cut power to the RF switch and float the control lines so it draws
 * ~0 uA in deep sleep. */
static void rf_switch_disable(void)
{
    gpio_reset_pin(WM_ANTENNA_CTRL_GPIO);
    gpio_set_direction(WM_ANTENNA_CTRL_GPIO, GPIO_MODE_INPUT);
    gpio_reset_pin(WM_ANTENNA_VDD_GPIO);
    gpio_set_direction(WM_ANTENNA_VDD_GPIO, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "RF switch off");
}
#endif

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

    /* Force a report on every meter (and battery, if enabled) every
     * WM_KEEPALIVE_PERIOD_S, so HA sees regular traffic even with no
     * water flow. */
    int64_t last_keepalive_us = now_us0;

    /* -1 forces the very first loop pass to publish the leak state,
     * the same way last_pushed[] = UINT64_MAX does for the meters. */
    int last_leak_pushed = -1;

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

        /* The probe is a level, not a pulse, so it just gets polled.
         * Only a change is pushed here; the keepalive below re-sends
         * the state whether it changed or not. */
        bool leak_now;
        leak_sensor_poll(&leak_now);
        if ((int)leak_now != last_leak_pushed) {
            zb_metering_update_leak(leak_now);
            last_leak_pushed = (int)leak_now;
        }

#if WM_BATTERY_MONITORING
        /* Sample the battery roughly every minute. */
        if ((now_us - last_batt_us) >= 60LL * 1000000LL) {
            uint32_t mv  = battery_read_mv();
            uint8_t  pct = battery_read_percent();
            zb_metering_update_battery(pct, mv);
            last_batt_us = now_us;
        }
#endif

        /* Hourly keepalive: re-push every meter (and battery) even if
         * nothing changed, so HA's availability tracker stays happy. */
        if ((now_us - last_keepalive_us) >=
            (int64_t)WM_KEEPALIVE_PERIOD_S * 1000000LL) {
            for (int i = 0; i < WM_NUM_METERS; i++) {
                uint64_t total_x1000 =
                    pulse_counter_get_total_liters_x1000(i);
                zb_metering_update_total(i, total_x1000);
                last_pushed[i] = total_x1000;
                ESP_LOGI(TAG, "keepalive: meter[%d] = %llu L (x1000)",
                         i, (unsigned long long)total_x1000);
            }
            last_leak_pushed = (int)leak_sensor_state();
            zb_metering_update_leak(last_leak_pushed != 0);
#if WM_BATTERY_MONITORING
            uint32_t mv  = battery_read_mv();
            uint8_t  pct = battery_read_percent();
            zb_metering_update_battery(pct, mv);
            last_batt_us = now_us;
#endif
            last_keepalive_us = now_us;
        }

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

static void deep_sleep_cycle(bool joined)
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

    /* leak_sensor_init() already took a debounced sample this wake, so
     * report that rather than paying another debounce window of awake
     * time. A leak appearing during sleep is what raised the GPIO wake;
     * a leak drying out is only ever noticed on a timer wake. */
    zb_metering_update_leak(leak_sensor_state());

#if WM_BATTERY_MONITORING
    uint32_t batt_mv  = battery_read_mv();
    uint8_t  batt_pct = battery_read_percent();
    zb_metering_update_battery(batt_pct, batt_mv);
#endif

    /* Only spend ~3 s draining the TX queue if we actually have a
     * network to send to. If join failed, the queue is empty and there
     * is no point keeping the radio hot. */
    if (joined) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    /* Add the leak probe to the wake mask. leak_sensor_wake_mask()
     * returns 0 while the probe is already wet, so we don't wake
     * straight back up on a leak we have just reported. */
    wake_mask |= leak_sensor_wake_mask();

    /* Configure all wake GPIOs + the keepalive timer and sleep. */
    esp_deep_sleep_enable_gpio_wakeup(wake_mask,
                                      ESP_GPIO_WAKEUP_GPIO_LOW);
    uint32_t sleep_s = joined ? WM_KEEPALIVE_PERIOD_S : WM_DEEPSLEEP_RETRY_S;
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);

    ESP_LOGI(TAG, "Entering deep sleep for up to %lu s (%s)",
             (unsigned long)sleep_s,
             joined ? "joined" : "join-retry backoff");
    rf_switch_disable();
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
    ESP_LOGI(TAG, "Mode: %s, %s%s%s",
             WM_POWER_USB ? "USB/Router" : "Battery/EndDevice",
             WM_USE_DEEP_SLEEP ? "deep-sleep" : "always-on",
             WM_BATTERY_MONITORING ? ", battery-mon" : "",
             WM_LEAK_SENSOR ? ", leak-sensor" : "");

    rf_switch_enable(WM_ANTENNA_EXTERNAL);

    ESP_ERROR_CHECK(storage_init());

    ESP_ERROR_CHECK(pulse_counter_init());
    for (int i = 0; i < WM_NUM_METERS; i++) {
        uint64_t saved = storage_load_liters_x1000(i);
        pulse_counter_set_initial_liters_x1000(i, saved);
    }

    ESP_ERROR_CHECK(leak_sensor_init());

#if WM_BATTERY_MONITORING
    ESP_ERROR_CHECK(battery_init());
#endif

    ESP_ERROR_CHECK(zb_metering_start());

#if WM_USE_DEEP_SLEEP
    /* Wait for join/rejoin before pushing reports + sleeping. Keep the
     * timeouts short: a warm rejoin from zb_storage is sub-second,
     * and a cold join either lands quickly (coordinator with permit-
     * join open) or won't land at all, in which case staying awake
     * just burns the battery. */
    uint32_t join_timeout_ms =
        (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED)
            ? WM_ZB_JOIN_TIMEOUT_COLD_MS
            : WM_ZB_JOIN_TIMEOUT_WARM_MS;
    bool joined = zb_metering_wait_joined(join_timeout_ms);
    if (!joined) {
        ESP_LOGW(TAG, "Not joined within %lu ms, sleeping longer to save battery",
                 (unsigned long)join_timeout_ms);
    }
    deep_sleep_cycle(joined);   /* never returns - sleeps the chip */
#else
    xTaskCreate(supervisor_task, "wm_sup", 4096, NULL, 4, NULL);
#endif
}
