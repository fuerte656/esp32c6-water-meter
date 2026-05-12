/* Battery voltage sensor for 3xAA configuration via voltage divider.
 *
 * Hardware:  Battery+ -- 100k -- GPIO 4 -- 100k -- GND
 * The midpoint reads half the battery voltage.
 *
 * Range:
 *   3 fresh alkaline AAs ~ 4.5 V -> divider 2.25 V -> ADC OK
 *   3 depleted AAs       ~ 3.0 V -> divider 1.50 V -> ADC OK
 *
 * If WM_BATTERY_MONITORING is 0 this file compiles to harmless stubs.
 */

#include "battery.h"
#include "wm_config.h"
#include "esp_log.h"



#if WM_BATTERY_MONITORING

static const char *TAG = "wm_batt";

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* GPIO 4 on ESP32-C6 is ADC1 channel 4. */
#define BATT_ADC_UNIT       ADC_UNIT_1
#define BATT_ADC_CHANNEL    ADC_CHANNEL_4
#define BATT_ADC_ATTEN      ADC_ATTEN_DB_12   /* up to ~3.3 V */
#define BATT_DIVIDER_NUM    2                 /* 100k+100k -> *2 */

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_ok = false;

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATT_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Calibration. Curve fitting is the C6's preferred scheme. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .chan     = BATT_ADC_CHANNEL,
        .atten    = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable, raw values only");
    }

    ESP_LOGI(TAG, "battery ADC ready on GPIO %d (CH %d)",
             WM_BATTERY_ADC_GPIO, BATT_ADC_CHANNEL);
    return ESP_OK;
}

uint32_t battery_read_mv(void)
{
    if (!s_adc) return 0;

    int raw = 0;
    if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK) {
        return 0;
    }
    int mv_at_pin = raw;
    if (s_cali_ok) {
        int cal_mv = 0;
        if (adc_cali_raw_to_voltage(s_cali, raw, &cal_mv) == ESP_OK) {
            mv_at_pin = cal_mv;
        }
    }
    /* Divider doubles the voltage. */
    return (uint32_t)mv_at_pin * BATT_DIVIDER_NUM;
}

uint8_t battery_read_percent(void)
{
    uint32_t mv = battery_read_mv();
    if (mv == 0) return 0;

    /* Linear map between 3000 mV (0%) and 4200 mV (100%). */
    if (mv <= 3000) return 0;
    if (mv >= 4200) return 100;
    return (uint8_t)(((mv - 3000) * 100) / (4200 - 3000));
}

#else  /* !WM_BATTERY_MONITORING - stubs */

esp_err_t battery_init(void)        { return ESP_OK; }
uint32_t  battery_read_mv(void)     { return 0; }
uint8_t   battery_read_percent(void){ return 0; }

#endif
