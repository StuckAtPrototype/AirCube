/**
 * @file zigbee.c
 * @brief Zigbee integration for AirCube
 *
 * Implements a Zigbee End Device that exposes environmental sensor data
 * via standard and custom ZCL clusters.
 *
 * Clusters on Endpoint 10:
 *   - Basic (0x0000)             : Device identity, SWBuildID (firmware version)
 *   - Identify (0x0003)          : Standard identify
 *   - Temperature Meas (0x0402)  : Actual temperature in 0.01 C
 *   - Humidity Meas (0x0405)     : Actual humidity in 0.01 %
 *   - Custom (0xFC01)            : eCO2, eTVOC, AQI
 *   - Analog Output (0x000D)     : LED brightness (0-100)
 *
 * @author StuckAtPrototype, LLC
 */

#include "zigbee.h"
#include "led.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_command.h"

static const char *TAG = "zigbee";

/* ── Endpoint & cluster configuration ────────────────────────────────── */

#define AIRCUBE_ENDPOINT            10

/* Custom cluster for air quality metrics (manufacturer-specific range) */
#define CUSTOM_CLUSTER_ID           0xFC01
#define ATTR_ECO2_ID                0x0000   /* uint16 – ppm   */
#define ATTR_ETVOC_ID               0x0001   /* uint16 – ppb   */
#define ATTR_AQI_ID                 0x0002   /* uint16 – index */

/* Analog Output cluster (0x000D) for brightness – standard cluster so
   ZCL Write Attributes from coordinators is handled natively by ZBOSS. */

/* Zigbee channel mask – scan all channels */
#define AIRCUBE_CHANNEL_MASK        ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/* ZCL string attributes: first byte is the string length */
#define MANUFACTURER_NAME           "\x10" "StuckAtPrototype"
#define MODEL_IDENTIFIER            "\x07" "AirCube"

/* ZCL char string: length byte + payload. Basic cluster SWBuildID max 16 chars (Zigbee 3.0). */
#define SW_BUILD_ZCL_MAX_CHARS      16
#define SW_BUILD_ZCL_BUF_LEN        (SW_BUILD_ZCL_MAX_CHARS + 1)

/* ── State ───────────────────────────────────────────────────────────── */

static volatile bool s_connected  = false;
static volatile bool s_pairing    = false;
static volatile bool s_rejoining  = false;
static TickType_t    s_pairing_start = 0;
static uint32_t      s_rejoin_backoff_ms = 0;
static uint8_t       s_sw_build_id[SW_BUILD_ZCL_BUF_LEN];

#define PAIRING_TIMEOUT_MS      60000   /* Auto-cancel pairing after 60 s */
#define REJOIN_BACKOFF_INIT_MS  1000    /* First rejoin attempt after 1 s  */
#define REJOIN_BACKOFF_MAX_MS   300000  /* Cap backoff at 5 minutes        */

/* ── Helpers ─────────────────────────────────────────────────────────── */

/** Convert float C to ZCL int16 (hundredths of a degree). */
static int16_t temp_to_zb(float temp_c)
{
    return (int16_t)(temp_c * 100.0f);
}

/** Convert float %RH to ZCL uint16 (hundredths of a percent). */
static uint16_t humidity_to_zb(float rh)
{
    return (uint16_t)(rh * 100.0f);
}

/** Zigbee ZCL char string (length-prefixed) from ESP-IDF app image version. */
static void init_sw_build_id(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    size_t n = strnlen(app_desc->version, sizeof(app_desc->version));
    if (n > SW_BUILD_ZCL_MAX_CHARS) {
        n = SW_BUILD_ZCL_MAX_CHARS;
    }
    s_sw_build_id[0] = (uint8_t)n;
    memcpy(&s_sw_build_id[1], app_desc->version, n);
}

static void report_attr(uint16_t cluster_id, uint16_t attr_id)
{
    esp_zb_zcl_report_attr_cmd_t report_cmd = { 0 };
    report_cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    report_cmd.zcl_basic_cmd.dst_endpoint = 1;
    report_cmd.zcl_basic_cmd.src_endpoint = AIRCUBE_ENDPOINT;
    report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    report_cmd.clusterID = cluster_id;
    report_cmd.manuf_specific = 0;
    report_cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    report_cmd.dis_default_resp = 1;
    report_cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    report_cmd.attributeID = attr_id;
    esp_zb_zcl_report_attr_cmd_req(&report_cmd);
}

/* ── Forward declarations ─────────────────────────────────────────────── */

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);

/* ── Rejoin helper (exponential backoff) ──────────────────────────────── */

static void schedule_rejoin(void)
{
    if (!s_rejoining) {
        s_rejoining = true;
        s_rejoin_backoff_ms = REJOIN_BACKOFF_INIT_MS;
    }
    ESP_LOGW(TAG, "Scheduling rejoin in %lu ms", (unsigned long)s_rejoin_backoff_ms);
    esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                           ESP_ZB_BDB_MODE_NETWORK_STEERING, s_rejoin_backoff_ms);
    s_rejoin_backoff_ms *= 2;
    if (s_rejoin_backoff_ms > REJOIN_BACKOFF_MAX_MS) {
        s_rejoin_backoff_ms = REJOIN_BACKOFF_MAX_MS;
    }
}

/* ── NVS pairing flag (survives the reboot caused by factory_reset) ─── */

#define NVS_NAMESPACE   "aircube"
#define NVS_KEY_PAIRING "zb_pair_req"

static void set_pairing_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PAIRING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool consume_pairing_flag(void)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool found = (nvs_get_u8(h, NVS_KEY_PAIRING, &val) == ESP_OK && val);
    nvs_erase_key(h, NVS_KEY_PAIRING);
    nvs_commit(h);
    nvs_close(h);
    return found;
}

/* ── ZED configuration macros (matching Espressif examples) ──────────── */

#define AIRCUBE_ZED_CONFIG()                                \
    {                                                       \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,               \
        .install_code_policy = false,                       \
        .nwk_cfg.zed_cfg = {                                \
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,    \
            .keep_alive = 3000,                             \
        },                                                  \
    }

#define AIRCUBE_RADIO_CONFIG()          \
    {                                   \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define AIRCUBE_HOST_CONFIG()           \
    {                                   \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }

/* ── Commissioning helper ────────────────────────────────────────────── */

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, ,
                        TAG, "Failed to start Zigbee commissioning");
}

/* ── Signal handler (called by the Zigbee stack) ─────────────────────── */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status   = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode",
                     esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                if (s_pairing || consume_pairing_flag()) {
                    s_pairing       = true;
                    s_pairing_start = xTaskGetTickCount();
                    ESP_LOGI(TAG, "Pairing requested – start network steering");
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    ESP_LOGI(TAG, "Factory-new device – idle until long-press pairing");
                }
            } else {
                ESP_LOGI(TAG, "Device rebooted – already commissioned");
                s_connected = true;
            }
        } else {
            ESP_LOGI(TAG, "Waiting for coordinator (%s), retrying",
                     esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network (Ext PAN: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, "
                     "PAN: 0x%04hx, CH: %d, Addr: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5],
                     extended_pan_id[4], extended_pan_id[3], extended_pan_id[2],
                     extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
            s_connected  = true;
            s_pairing    = false;
            s_rejoining  = false;
            s_rejoin_backoff_ms = REJOIN_BACKOFF_INIT_MS;
        } else {
            if (s_pairing &&
                (xTaskGetTickCount() - s_pairing_start) < pdMS_TO_TICKS(PAIRING_TIMEOUT_MS)) {
                ESP_LOGI(TAG, "Network steering failed (status: %s), retrying",
                         esp_err_to_name(err_status));
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            } else if (s_rejoining) {
                ESP_LOGW(TAG, "Rejoin steering failed (%s), backing off",
                         esp_err_to_name(err_status));
                schedule_rejoin();
            } else {
                ESP_LOGW(TAG, "Network steering stopped – %s",
                         s_pairing ? "timed out" : "no pairing requested");
                s_pairing = false;
            }
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE: {
        esp_zb_zdo_signal_leave_params_t *leave_params =
            (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        uint8_t leave_type = leave_params ? leave_params->leave_type : 0xFF;
        ESP_LOGW(TAG, "Left network (leave_type: 0x%x)", leave_type);
        s_connected = false;
        if (!s_pairing) {
            schedule_rejoin();
        }
        break;
    }

    case ESP_ZB_NLME_STATUS_INDICATION: {
        esp_zb_zdo_signal_nwk_status_indication_params_t *nwk_status =
            (esp_zb_zdo_signal_nwk_status_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        uint8_t status  = nwk_status ? nwk_status->status       : 0xFF;
        uint16_t addr   = nwk_status ? nwk_status->network_addr : 0xFFFF;
        ESP_LOGW(TAG, "Network status indication: 0x%02x, addr: 0x%04x", status, addr);
        if (status == ESP_ZB_NWK_COMMAND_STATUS_PARENT_LINK_FAILURE) {
            ESP_LOGW(TAG, "Parent link failure – marking disconnected");
            s_connected = false;
            if (!s_rejoining && !s_pairing) {
                schedule_rejoin();
            }
        }
        break;
    }

    case ESP_ZB_BDB_SIGNAL_TC_REJOIN_DONE:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Trust Center rejoin succeeded");
            s_connected  = true;
            s_rejoining  = false;
            s_rejoin_backoff_ms = REJOIN_BACKOFF_INIT_MS;
        } else {
            ESP_LOGW(TAG, "Trust Center rejoin failed (%s)", esp_err_to_name(err_status));
            s_connected = false;
            if (!s_pairing) {
                schedule_rejoin();
            }
        }
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

/* ── Cluster list creation ───────────────────────────────────────────── */

static esp_zb_cluster_list_t *create_cluster_list(void)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    /* ---- Basic cluster (mandatory: identity + firmware version for coordinators) ---- */
    init_sw_build_id();
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUFACTURER_NAME));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_IDENTIFIER));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void *)s_sw_build_id));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cluster_list,
        basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* ---- Identify cluster (mandatory) ---- */
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cluster_list,
        esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* ---- Temperature Measurement cluster (0x0402) ---- */
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = 0,
        .min_value = -1000,     /* -10.00 C */
        .max_value = 8000,      /*  80.00 C */
    };
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list,
        esp_zb_temperature_meas_cluster_create(&temp_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* ---- Relative Humidity Measurement cluster (0x0405) ---- */
    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = 0,
        .min_value = 0,         /*   0.00 % */
        .max_value = 10000,     /* 100.00 % */
    };
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list,
        esp_zb_humidity_meas_cluster_create(&hum_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* ---- Custom cluster 0xFC01 (eCO2, eTVOC, AQI) ---- */
    esp_zb_attribute_list_t *custom_cluster = esp_zb_zcl_attr_list_create(CUSTOM_CLUSTER_ID);

    uint16_t default_val = 0;

    ESP_ERROR_CHECK(esp_zb_custom_cluster_add_custom_attr(custom_cluster,
        ATTR_ECO2_ID, ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &default_val));

    ESP_ERROR_CHECK(esp_zb_custom_cluster_add_custom_attr(custom_cluster,
        ATTR_ETVOC_ID, ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &default_val));

    ESP_ERROR_CHECK(esp_zb_custom_cluster_add_custom_attr(custom_cluster,
        ATTR_AQI_ID, ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        &default_val));

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cluster_list,
        custom_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* ---- Analog Output cluster 0x000D (brightness, writable) ---- */
    esp_zb_analog_output_cluster_cfg_t ao_cfg = {
        .out_of_service = false,
        .present_value  = 60.0f,
        .status_flags   = 0,
    };
    esp_zb_attribute_list_t *ao_cluster =
        esp_zb_analog_output_cluster_create(&ao_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_analog_output_cluster(cluster_list,
        ao_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    return cluster_list;
}

/* ── Reporting configuration ─────────────────────────────────────────── */

static void configure_reporting(void)
{
    /* Temperature: report every 60s max, or on 0.50 C change */
    esp_zb_zcl_reporting_info_t temp_rpt = {
        .direction          = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep                 = AIRCUBE_ENDPOINT,
        .cluster_id         = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        .cluster_role       = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval     = 1,
        .u.send_info.max_interval     = 60,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = 60,
        .u.send_info.delta.u16        = 50,    /* 0.50 C */
        .attr_id            = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
        .manuf_code         = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&temp_rpt);

    /* Humidity: report every 60s max, or on 1.0 %RH change */
    esp_zb_zcl_reporting_info_t hum_rpt = {
        .direction          = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep                 = AIRCUBE_ENDPOINT,
        .cluster_id         = ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
        .cluster_role       = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval     = 1,
        .u.send_info.max_interval     = 60,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = 60,
        .u.send_info.delta.u16        = 100,   /* 1.00 %RH */
        .attr_id            = ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
        .manuf_code         = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&hum_rpt);

    /* eCO2: report every 60s max, or on 50 ppm change */
    esp_zb_zcl_reporting_info_t eco2_rpt = {
        .direction          = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep                 = AIRCUBE_ENDPOINT,
        .cluster_id         = CUSTOM_CLUSTER_ID,
        .cluster_role       = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval     = 1,
        .u.send_info.max_interval     = 60,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = 60,
        .u.send_info.delta.u16        = 50,
        .attr_id            = ATTR_ECO2_ID,
        .manuf_code         = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&eco2_rpt);

    /* eTVOC: report every 60s max, or on 10 ppb change */
    esp_zb_zcl_reporting_info_t etvoc_rpt = {
        .direction          = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep                 = AIRCUBE_ENDPOINT,
        .cluster_id         = CUSTOM_CLUSTER_ID,
        .cluster_role       = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval     = 1,
        .u.send_info.max_interval     = 60,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = 60,
        .u.send_info.delta.u16        = 10,
        .attr_id            = ATTR_ETVOC_ID,
        .manuf_code         = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&etvoc_rpt);

    /* AQI: report every 60s max, or on 5-point change */
    esp_zb_zcl_reporting_info_t aqi_rpt = {
        .direction          = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep                 = AIRCUBE_ENDPOINT,
        .cluster_id         = CUSTOM_CLUSTER_ID,
        .cluster_role       = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .dst.profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval     = 1,
        .u.send_info.max_interval     = 60,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = 60,
        .u.send_info.delta.u16        = 5,
        .attr_id            = ATTR_AQI_ID,
        .manuf_code         = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
    };
    esp_zb_zcl_update_reporting_info(&aqi_rpt);

}

static void apply_zigbee_tx_power(void)
{
#if defined(CONFIG_AIRCUBE_ZB_TX_POWER_DBM)
#if defined(ESP_ZB_VER_MAJOR) && defined(ESP_ZB_VER_MINOR) && \
    ((ESP_ZB_VER_MAJOR > 1) || (ESP_ZB_VER_MAJOR == 1 && ESP_ZB_VER_MINOR >= 6))
    const int8_t requested_dbm = (int8_t)CONFIG_AIRCUBE_ZB_TX_POWER_DBM;
    int8_t applied_dbm = 0;

    esp_zb_set_tx_power(requested_dbm);
    esp_zb_get_tx_power(&applied_dbm);

    if (applied_dbm != requested_dbm) {
        ESP_LOGW(TAG, "Requested Zigbee TX power %d dBm, applied %d dBm",
                 requested_dbm, applied_dbm);
    } else {
        ESP_LOGI(TAG, "Zigbee TX power set to %d dBm", applied_dbm);
    }
#else
    ESP_LOGW(TAG, "Zigbee TX power config is not supported by this ESP Zigbee SDK");
#endif
#endif
}

/* ── Action handler (brightness writes via Analog Output cluster) ─────── */

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID && message) {
        const esp_zb_zcl_set_attr_value_message_t *m =
            (const esp_zb_zcl_set_attr_value_message_t *)message;

        if (m->info.dst_endpoint == AIRCUBE_ENDPOINT &&
            m->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT &&
            m->attribute.id == ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID)
        {
            float raw = *(float *)m->attribute.data.value;
            if (raw < 0.0f)   raw = 0.0f;
            if (raw > 100.0f) raw = 100.0f;
            led_set_intensity(raw / 100.0f);
            ESP_LOGI(TAG, "Brightness set to %.0f%% via Zigbee", raw);
        }
    }
    return ESP_OK;
}

/* ── Zigbee main task ────────────────────────────────────────────────── */

static void esp_zb_task(void *pvParameters)
{
    /* Initialize Zigbee stack as End Device */
    esp_zb_cfg_t zb_nwk_cfg = AIRCUBE_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    /* Create endpoint 10 with all our clusters */
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint       = AIRCUBE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id  = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, create_cluster_list(), endpoint_config);

    /* Register the device */
    esp_zb_device_register(ep_list);

    /* Set up automatic attribute reporting */
    configure_reporting();

    /* Use all channels for network steering */
    esp_zb_set_primary_network_channel_set(AIRCUBE_CHANNEL_MASK);

    /* Apply configured Zigbee TX power before stack start */
    apply_zigbee_tx_power();

    /* Handle attribute writes from coordinator (brightness via Analog Output) */
    esp_zb_core_action_handler_register(zb_action_handler);

    /* Start the Zigbee stack (false = not coordinator) */
    ESP_ERROR_CHECK(esp_zb_start(false));

    ESP_LOGI(TAG, "Zigbee stack started – waiting for network");

    /* Run the Zigbee main loop (never returns) */
    esp_zb_stack_main_loop();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void zigbee_init(void)
{
    /* Configure platform for native radio (ESP32-H2 on-chip 802.15.4) */
    esp_zb_platform_config_t config = {
        .radio_config = AIRCUBE_RADIO_CONFIG(),
        .host_config  = AIRCUBE_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    /* Launch the Zigbee task */
    xTaskCreate(esp_zb_task, "zigbee_main", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Zigbee initialized");
}

void zigbee_update_sensors(float temp_c, float humidity, int eco2, int etvoc, int aqi)
{
    if (!s_connected) {
        return;     /* Don't update attributes until we've joined a network */
    }

    /* Convert to ZCL units */
    int16_t  zb_temp  = temp_to_zb(temp_c);
    uint16_t zb_hum   = humidity_to_zb(humidity);
    uint16_t zb_eco2  = (uint16_t)eco2;
    uint16_t zb_etvoc = (uint16_t)etvoc;
    uint16_t zb_aqi   = (uint16_t)aqi;

    /* Lock the Zigbee stack while writing attributes */
    esp_zb_lock_acquire(portMAX_DELAY);

    /* Standard clusters */
    esp_zb_zcl_set_attribute_val(AIRCUBE_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &zb_temp, false);

    esp_zb_zcl_set_attribute_val(AIRCUBE_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, &zb_hum, false);

    /* Custom cluster (0xFC01) */
    esp_zb_zcl_set_attribute_val(AIRCUBE_ENDPOINT,
        CUSTOM_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ATTR_ECO2_ID, &zb_eco2, false);

    esp_zb_zcl_set_attribute_val(AIRCUBE_ENDPOINT,
        CUSTOM_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ATTR_ETVOC_ID, &zb_etvoc, false);

    esp_zb_zcl_set_attribute_val(AIRCUBE_ENDPOINT,
        CUSTOM_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ATTR_AQI_ID, &zb_aqi, false);

    /* Sync current brightness to Analog Output cluster (covers button changes) */
    float zb_brightness = led_get_intensity() * 100.0f;
    esp_zb_zcl_set_attribute_val(AIRCUBE_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, &zb_brightness, false);

    /* One-shot attribute reports to ensure coordinator updates */
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID);
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
    report_attr(CUSTOM_CLUSTER_ID, ATTR_ECO2_ID);
    report_attr(CUSTOM_CLUSTER_ID, ATTR_ETVOC_ID);
    report_attr(CUSTOM_CLUSTER_ID, ATTR_AQI_ID);

    esp_zb_lock_release();
}

bool zigbee_is_connected(void)
{
    return s_connected;
}

void zigbee_start_pairing(void)
{
    ESP_LOGI(TAG, "Manual pairing requested");
    s_pairing       = true;
    s_connected     = false;
    s_pairing_start = xTaskGetTickCount();

    esp_zb_lock_acquire(portMAX_DELAY);
    if (esp_zb_bdb_is_factory_new()) {
        ESP_LOGI(TAG, "Already factory-new – starting network steering directly");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    } else {
        ESP_LOGI(TAG, "Clearing credentials – device will reboot");
        set_pairing_flag();
        esp_zb_factory_reset();
    }
    esp_zb_lock_release();
}

bool zigbee_is_pairing(void)
{
    if (!s_pairing) {
        return false;
    }
    /* Auto-timeout: stop the visual indicator after PAIRING_TIMEOUT_MS */
    if ((xTaskGetTickCount() - s_pairing_start) > pdMS_TO_TICKS(PAIRING_TIMEOUT_MS)) {
        s_pairing = false;
        ESP_LOGW(TAG, "Pairing timed out");
        return false;
    }
    return true;
}
