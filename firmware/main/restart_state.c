#include "restart_state.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "restart_state";

#define RESTART_STATE_MAGIC   0x41435253u  // "ACRS"
#define RESTART_STATE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    restart_visual_state_t visual;
    bool visual_valid;
    bool scd41_running;
    uint8_t reserved[2];
    uint32_t checksum;
} retained_state_t;

// Deliberately not initialized by the C runtime. RTC memory survives MCU resets
// but is lost when power is removed, so it carries state without wearing flash.
RTC_NOINIT_ATTR static retained_state_t s_retained;
static bool s_warm_boot;

static uint32_t state_checksum(const retained_state_t *state)
{
    const uint8_t *bytes = (const uint8_t *)state;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(retained_state_t, checksum); i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool state_valid(void)
{
    return s_retained.magic == RESTART_STATE_MAGIC &&
           s_retained.version == RESTART_STATE_VERSION &&
           s_retained.size == sizeof(s_retained) &&
           s_retained.checksum == state_checksum(&s_retained);
}

static void state_commit(void)
{
    s_retained.checksum = state_checksum(&s_retained);
}

static bool reset_kept_peripheral_power(esp_reset_reason_t reason)
{
    // A brownout can also disturb the sensor supply, so handle it as a cold boot.
    return reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT &&
           reason != ESP_RST_UNKNOWN;
}

void restart_state_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    s_warm_boot = state_valid() && reset_kept_peripheral_power(reason);

    if (!s_warm_boot) {
        memset(&s_retained, 0, sizeof(s_retained));
        s_retained.magic = RESTART_STATE_MAGIC;
        s_retained.version = RESTART_STATE_VERSION;
        s_retained.size = sizeof(s_retained);
        state_commit();
    }

    ESP_LOGI(TAG, "%s boot (reset reason %d, visual=%d, SCD41 running=%d)",
             s_warm_boot ? "Warm" : "Cold", reason,
             s_warm_boot && s_retained.visual_valid,
             s_warm_boot && s_retained.scd41_running);
}

bool restart_state_is_warm_boot(void)
{
    return s_warm_boot;
}

void restart_state_update_visual(const restart_visual_state_t *state)
{
    if (state == NULL) {
        return;
    }

    s_retained.visual = *state;
    s_retained.visual_valid = true;
    state_commit();
}

bool restart_state_get_visual(restart_visual_state_t *state)
{
    if (state == NULL || !s_warm_boot || !s_retained.visual_valid) {
        return false;
    }

    *state = s_retained.visual;
    return true;
}

void restart_state_set_scd41_running(bool running)
{
    s_retained.scd41_running = running;
    state_commit();
}

bool restart_state_should_resume_scd41(void)
{
    return s_warm_boot && s_retained.scd41_running;
}
