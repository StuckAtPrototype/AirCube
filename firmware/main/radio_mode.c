/**
 * @file radio_mode.c
 * @brief BLE-first radio mode selection (see radio_mode.h)
 */

#include "radio_mode.h"
#include "zigbee.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "radio_mode";

#define NVS_NAMESPACE      "aircube"
#define NVS_KEY_ZB_JOINED  "zb_joined"
/* Note: "zb_pair_req" is owned by zigbee.c (set on re-pair factory reset).
 * We share it so a BLE->Zigbee pairing reboot triggers steering the same
 * way a Zigbee-mode re-pair reboot does. */
#define NVS_KEY_PAIRING    "zb_pair_req"

static radio_mode_t s_mode = RADIO_MODE_BLE;

static bool nvs_get_flag(const char *key)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool set = (nvs_get_u8(h, key, &val) == ESP_OK && val);
    nvs_close(h);
    return set;
}

static void nvs_set_flag(const char *key, bool value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for %s", key);
        return;
    }
    if (value) {
        nvs_set_u8(h, key, 1);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

void radio_mode_init(void)
{
    bool joined  = nvs_get_flag(NVS_KEY_ZB_JOINED);
    bool pairing = nvs_get_flag(NVS_KEY_PAIRING);

    s_mode = (joined || pairing) ? RADIO_MODE_ZIGBEE : RADIO_MODE_BLE;

    ESP_LOGI(TAG, "Boot radio mode: %s (zb_joined=%d, pairing_requested=%d)",
             s_mode == RADIO_MODE_ZIGBEE ? "ZIGBEE" : "BLE", joined, pairing);
}

radio_mode_t radio_mode_get(void)
{
    return s_mode;
}

void radio_mode_start_pairing(void)
{
    if (s_mode == RADIO_MODE_ZIGBEE) {
        /* Already in Zigbee mode: normal re-pair flow (zigbee.c handles
         * factory reset + reboot if we were previously commissioned). */
        zigbee_start_pairing();
        return;
    }

    /* BLE mode: request pairing and reboot into Zigbee mode. Steering
     * starts automatically because zigbee.c consumes the pairing flag on
     * its factory-new first start. */
    ESP_LOGI(TAG, "Pairing requested from BLE mode - rebooting into Zigbee mode");
    nvs_set_flag(NVS_KEY_PAIRING, true);
    esp_restart();
}

void radio_mode_set_joined(bool joined)
{
    ESP_LOGI(TAG, "Zigbee joined flag -> %d", joined);
    nvs_set_flag(NVS_KEY_ZB_JOINED, joined);
}

void radio_mode_revert_to_ble(void)
{
    ESP_LOGW(TAG, "Reverting to BLE mode - rebooting");
    nvs_set_flag(NVS_KEY_ZB_JOINED, false);
    nvs_set_flag(NVS_KEY_PAIRING, false);
    esp_restart();
}
