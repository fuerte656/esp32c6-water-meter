/* Zigbee Metering cluster wiring for the dual-meter water counter.
 *
 * Registers one endpoint per physical meter (WM_NUM_METERS), each
 * exposing Basic / Identify / Metering. PowerCfg lives on the first
 * endpoint only because there's a single battery.
 *
 * Adapts to the WM_POWER_USB / WM_DEEP_SLEEP / WM_BATTERY_MONITORING
 * flags in wm_config.h:
 *   - Router on USB power, End Device on battery.
 *   - Optional genPowerCfg cluster when battery monitoring is on.
 *   - currentSummDelivered always carries the REPORTING access flag
 *     and Multiplier/Divisor are exposed as Z2M expects (1/1000 ->
 *     stored value is in liters, m^3 = value/1000).
 */

#include "zb_metering.h"
#include "wm_config.h"

#include "esp_zigbee_core.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "wm_zb";

#define ZCL_METERING_CURRENT_SUMMATION_DELIVERED_ID  0x0000

static const uint8_t s_endpoints[WM_NUM_METERS] = {
    WM_ESP_ZB_ENDPOINT_1,
    WM_ESP_ZB_ENDPOINT_2,
};

static bool s_joined = false;

static esp_zb_uint48_t s_summation[WM_NUM_METERS] = {0};

#if WM_BATTERY_MONITORING
/* 0xFF = "unknown" per ZCL until the first sample arrives. */
static uint8_t s_batt_percent_zcl  = 0xFF; /* ZCL units = 0.5 %, 0xFF=unknown */
static uint8_t s_batt_voltage_x100 = 0xFF; /* ZCL units = 100 mV, 0xFF=unknown */
#endif

/* Length-prefixed Zigbee strings, built at runtime to avoid C-literal
 * encoding ambiguity that some esp-zigbee-lib versions choke on. */
static char s_manuf[16];
static char s_model[32];

/* ---------- helpers ---------------------------------------------------- */

static void pack_uint48(uint64_t v, esp_zb_uint48_t *out)
{
    out->low  = (uint32_t)(v & 0xFFFFFFFFULL);
    out->high = (uint16_t)((v >> 32) & 0xFFFFULL);
}

__attribute__((unused))
static void start_top_level_commissioning_cb(uint8_t mode)
{
    (void)esp_zb_bdb_start_top_level_commissioning(mode);
}

static void build_zcl_string(char *dest, size_t dest_size, const char *src)
{
    size_t n = strlen(src);
    if (n > dest_size - 1) n = dest_size - 1;
    dest[0] = (char)n;
    memcpy(&dest[1], src, n);
}

/* ---------- cluster construction --------------------------------------- */

static esp_zb_cluster_list_t *create_clusters(int idx)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    /* Basic cluster */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = WM_POWER_USB ? 0x01 : 0x03,  /* mains vs battery */
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manuf);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Identify cluster */
    esp_zb_identify_cluster_cfg_t ident_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cluster_list,
        esp_zb_identify_cluster_create(&ident_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Metering cluster, built manually so currentSummDelivered carries
     * the REPORTING access flag. */
    esp_zb_attribute_list_t *metering = esp_zb_zcl_attr_list_create(
        ESP_ZB_ZCL_CLUSTER_ID_METERING);

    /* Per-meter init summation so each endpoint owns its own storage. */
    static esp_zb_uint48_t init_summation[WM_NUM_METERS] = {0};
    static uint8_t  status_attr = 0x00;
    static uint8_t  uom_attr    = 0x01;       /* m^3 */
    static uint8_t  fmt_attr    = 0x40;       /* 4 digits left, 0 right */
    static uint8_t  device_type = 0x02;       /* Water */
    static uint32_t multiplier  = 1;
    static uint32_t divisor     = 1000;       /* stored in L, m^3 = L/1000 */

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ZCL_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U48,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &init_summation[idx]);

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_STATUS_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &status_attr);

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &uom_attr);

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &fmt_attr);

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
        ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &device_type);

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U24,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &multiplier);

    esp_zb_cluster_add_attr(metering,
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
        ESP_ZB_ZCL_ATTR_TYPE_U24,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &divisor);

    esp_zb_cluster_list_add_metering_cluster(cluster_list, metering,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

#if WM_BATTERY_MONITORING
    /* Power Configuration cluster (0x0001) - only on first endpoint. */
    if (idx == 0) {
        esp_zb_attribute_list_t *power = esp_zb_zcl_attr_list_create(
            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG);

        esp_zb_cluster_add_attr(power,
            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            0x0020,                                    /* BatteryVoltage */
            ESP_ZB_ZCL_ATTR_TYPE_U8,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &s_batt_voltage_x100);

        esp_zb_cluster_add_attr(power,
            ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            0x0021,                                    /* BatteryPercentageRemaining */
            ESP_ZB_ZCL_ATTR_TYPE_U8,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &s_batt_percent_zcl);

        esp_zb_cluster_list_add_power_config_cluster(cluster_list, power,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    }
#endif

    return cluster_list;
}

static void register_endpoint(void)
{
    build_zcl_string(s_manuf, sizeof(s_manuf), "DIY");
    build_zcl_string(s_model, sizeof(s_model), "ESP32C6.WaterMeter");

    ESP_LOGI(TAG, "Setting Manufacturer='%.*s' (len=%d), Model='%.*s' (len=%d)",
             (int)s_manuf[0], &s_manuf[1], (int)s_manuf[0],
             (int)s_model[0], &s_model[1], (int)s_model[0]);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    for (int i = 0; i < WM_NUM_METERS; i++) {
        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = s_endpoints[i],
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id  = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
            .app_device_version = 0,
        };
        esp_zb_ep_list_add_ep(ep_list, create_clusters(i), ep_cfg);
    }

    esp_zb_device_register(ep_list);
}

/* ---------- ZDO signal handler ---------------------------------------- */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t  *p_sg_p   = signal_struct->p_app_signal;
    esp_err_t  err      = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee stack initialized, starting top-level commissioning");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Started, joining/rejoining (PAN ID 0x%04hx, channel %d)",
                        esp_zb_get_pan_id(), esp_zb_get_current_channel());
                if (esp_zb_bdb_is_factory_new()) {
                    ESP_LOGI(TAG, "Factory new -> network steering");
                    esp_zb_bdb_start_top_level_commissioning(
                        ESP_ZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    s_joined = true;
                    ESP_LOGI(TAG, "Already on network -> s_joined=1");
                }
            } else {
                ESP_LOGW(TAG, "Start failed: %s, retrying", esp_err_to_name(err));
                esp_zb_scheduler_alarm(start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Joined network: PAN ID 0x%04hx, channel %d -> s_joined=1",
                        esp_zb_get_pan_id(), esp_zb_get_current_channel());
                s_joined = true;
            } else {
                ESP_LOGW(TAG, "Steering failed: %s, retrying in 1s",
                         esp_err_to_name(err));
                esp_zb_scheduler_alarm(start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
            break;

        default:
            /* Verbose only; bring back to LOGD if you don't want chatter. */
            ESP_LOGD(TAG, "ZDO signal: %s (0x%x), status %s",
                    esp_zb_zdo_signal_to_string(sig), sig, esp_err_to_name(err));
            break;
    }
}

/* ---------- Public API ------------------------------------------------- */

void zb_metering_update_total(int idx, uint64_t liters_x1000)
{
    if (idx < 0 || idx >= WM_NUM_METERS) return;

    uint64_t liters = liters_x1000 / 1000ULL;
    pack_uint48(liters, &s_summation[idx]);

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_status_t st = esp_zb_zcl_set_attribute_val(
        s_endpoints[idx],
        ESP_ZB_ZCL_CLUSTER_ID_METERING,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZCL_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        &s_summation[idx], false);

    esp_err_t send_err = ESP_ERR_NOT_SUPPORTED;
    if (s_joined) {
        esp_zb_zcl_report_attr_cmd_t report = {
            .zcl_basic_cmd = {
                .src_endpoint = s_endpoints[idx],
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
            .clusterID    = ESP_ZB_ZCL_CLUSTER_ID_METERING,
            .manuf_code   = 0,
            .attributeID  = ZCL_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        };
        report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
        send_err = esp_zb_zcl_report_attr_cmd_req(&report);
    }
    esp_zb_lock_release();

    static uint64_t last_logged[WM_NUM_METERS] = { UINT64_MAX, UINT64_MAX };
    if (liters != last_logged[idx]) {
        ESP_LOGI(TAG, "meter[%d] summation = %llu L (set=0x%x, joined=%d, send=%s)",
                 idx, (unsigned long long)liters, st, s_joined,
                 esp_err_to_name(send_err));
        last_logged[idx] = liters;
    }
}

void zb_metering_update_battery(uint8_t percent, uint32_t mv)
{
#if WM_BATTERY_MONITORING
    /* ZCL "BatteryPercentageRemaining" is in units of 0.5 %. */
    s_batt_percent_zcl = (percent > 100) ? 200 : (uint8_t)(percent * 2);

    /* ZCL "BatteryVoltage" is in units of 100 mV, clamped to 0..0xFE.
     * 0xFF means "unknown" - leave it that way if we have no reading. */
    if (mv == 0) {
        s_batt_voltage_x100 = 0xFF;
    } else {
        uint32_t v = (mv + 50) / 100;   /* round to nearest 100 mV */
        if (v > 0xFE) v = 0xFE;
        s_batt_voltage_x100 = (uint8_t)v;
    }

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_set_attribute_val(
        WM_ESP_ZB_ENDPOINT_1,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        0x0021,
        &s_batt_percent_zcl, false);

    esp_zb_zcl_set_attribute_val(
        WM_ESP_ZB_ENDPOINT_1,
        ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        0x0020,
        &s_batt_voltage_x100, false);

    if (s_joined) {
        esp_zb_zcl_report_attr_cmd_t report = {
            .zcl_basic_cmd  = { .src_endpoint = WM_ESP_ZB_ENDPOINT_1 },
            .address_mode   = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
            .clusterID      = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            .manuf_code     = 0,
            .attributeID    = 0x0021,
            .direction      = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        };
        esp_zb_zcl_report_attr_cmd_req(&report);

        report.attributeID = 0x0020;
        esp_zb_zcl_report_attr_cmd_req(&report);
    }
    esp_zb_lock_release();

    ESP_LOGI(TAG, "battery: %u%% (%lu mV)",
             percent, (unsigned long)mv);
#else
    (void)percent;
    (void)mv;
#endif
}

/* ---------- Task / start ---------------------------------------------- */

static void zb_task(void *arg)
{
    esp_zb_cfg_t zb_cfg = {
#if WM_ZB_ROLE_ROUTER
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg    = { .max_children = 10 },
#else
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,    /* ms; parents buffer for sleeping ED */
        },
#endif
    };
    esp_zb_init(&zb_cfg);

    /* Force IEEE address from EFUSE if stack didn't pick it up. */
    esp_zb_ieee_addr_t addr;
    esp_zb_get_long_address(addr);
    bool addr_zero = true;
    for (int i = 0; i < 8; i++) if (addr[i] != 0) { addr_zero = false; break; }
    if (addr_zero) {
        uint8_t mac[8] = {0};
        esp_read_mac(mac, ESP_MAC_IEEE802154);
        esp_zb_ieee_addr_t fixed;
        for (int i = 0; i < 8; i++) fixed[i] = mac[7 - i];
        esp_zb_set_long_address(fixed);
        ESP_LOGI(TAG, "Set IEEE from EFUSE: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 fixed[7], fixed[6], fixed[5], fixed[4],
                 fixed[3], fixed[2], fixed[1], fixed[0]);
    }

    register_endpoint();

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

esp_err_t zb_metering_start(void)
{
    esp_zb_platform_config_t plat = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&plat));

    BaseType_t ok = xTaskCreate(zb_task, "zb_main", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
