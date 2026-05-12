/* PCNT-based pulse counter for the water meter.
 *
 * In always-on modes (USB router, ED no-sleep) the PCNT peripheral
 * counts hardware pulses with a glitch filter, and a watchpoint
 * callback applies a software debounce on top.
 *
 * In deep-sleep mode the PCNT is off most of the time (the chip is
 * sleeping), so wakes from GPIO call pulse_counter_increment_one()
 * manually after a software debounce.
 *
 * The cumulative total lives in RTC slow memory so it survives deep
 * sleep without an NVS write per pulse. NVS is still used as the
 * persistent backup against power loss.
 */

#include "pulse_counter.h"
#include "wm_config.h"

#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "wm_pcnt";

/* Cumulative total in liters * 1000. Lives in RTC slow memory so it
 * survives deep sleep. RTC slow memory is initialised to 0 only on a
 * cold boot (power-on / external reset); it persists across deep
 * sleep wake-ups. */
RTC_DATA_ATTR static uint64_t s_total_x1000 = 0;

/* Set by pulse_counter_set_initial_liters_x1000. Used by
 * pulse_counter_take so we can return cumulative-from-baseline. */
static atomic_int s_pulses_since_take = 0;

#if !WM_USE_DEEP_SLEEP
static pcnt_unit_handle_t s_unit = NULL;
static pcnt_channel_handle_t s_chan = NULL;
static int64_t s_last_pulse_us = 0;
#endif

/* ---------- always-on (PCNT) variant ---------- */

#if !WM_USE_DEEP_SLEEP

static bool IRAM_ATTR pcnt_watch_cb(pcnt_unit_handle_t unit,
                                    const pcnt_watch_event_data_t *edata,
                                    void *user_ctx)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_pulse_us) >=
        (int64_t)WM_SW_DEBOUNCE_MS * 1000LL)
    {
        s_last_pulse_us = now;
        atomic_fetch_add(&s_pulses_since_take, 1);
        s_total_x1000 += WM_LITERS_PER_PULSE_X1000;
    }
    /* Reset the count so we can fire the watchpoint again on next
     * pulse. */
    pcnt_unit_clear_count(unit);
    return false;
}

esp_err_t pulse_counter_init(void)
{
    ESP_LOGI(TAG, "init pcnt on GPIO %d, glitch %d ns, sw debounce %d ms",
             WM_PULSE_GPIO, WM_PCNT_GLITCH_NS, WM_SW_DEBOUNCE_MS);

    pcnt_unit_config_t unit_cfg = {
        .high_limit = 10000,
        .low_limit  = -1,
        .flags.accum_count = false,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));

    pcnt_glitch_filter_config_t filt = {
        .max_glitch_ns = WM_PCNT_GLITCH_NS,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filt));

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num  = WM_PULSE_GPIO,
        .level_gpio_num = -1,
        .flags.io_loop_back = false,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_cfg, &s_chan));

    /* Count on falling edges only (reed switch closes -> GND). */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE));

    /* Internal pull-up on the input pin (reed switch shorts to GND). */
    gpio_pullup_en(WM_PULSE_GPIO);

    /* Watchpoint at +1 so we get a callback every pulse. */
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_unit, 1));

    pcnt_event_callbacks_t cbs = { .on_reach = pcnt_watch_cb };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(s_unit, &cbs, NULL));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));
    return ESP_OK;
}

#else  /* WM_USE_DEEP_SLEEP */

esp_err_t pulse_counter_init(void)
{
    ESP_LOGI(TAG, "deep-sleep mode: PCNT not used; pulses counted via GPIO wake");
    /* Configure GPIO as input + pull-up so it idles high. */
    gpio_set_direction(WM_PULSE_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(WM_PULSE_GPIO);
    return ESP_OK;
}

#endif

/* ---------- common API ---------- */

void pulse_counter_set_initial_liters_x1000(uint64_t value)
{
    /* Only seed from NVS on cold boot. RTC RAM persists across
     * deep-sleep wakes, so we must NOT clobber it then. */
    if (s_total_x1000 == 0) {
        s_total_x1000 = value;
        ESP_LOGI(TAG, "restored initial total: %llu (liters x1000)",
                 (unsigned long long)value);
    } else {
        ESP_LOGI(TAG, "RTC RAM already has %llu, NVS=%llu, keeping RTC",
                 (unsigned long long)s_total_x1000,
                 (unsigned long long)value);
    }
}

int pulse_counter_take(void)
{
    int n = atomic_exchange(&s_pulses_since_take, 0);
    return n;
}

uint64_t pulse_counter_get_total_liters_x1000(void)
{
    return s_total_x1000;
}

void pulse_counter_increment_one(void)
{
    s_total_x1000 += WM_LITERS_PER_PULSE_X1000;
    atomic_fetch_add(&s_pulses_since_take, 1);
}
