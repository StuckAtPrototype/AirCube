//
// Pro-only lux-based auto-dim for LED brightness.
//

#include "auto_dim.h"
#include "button.h"
#include "device_model.h"
#include "led.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "auto_dim";

#define NVS_NAMESPACE "aircube"
#define NVS_KEY_AUTO_DIM_EN     "auto_dim_en"
#define NVS_KEY_NIGHT_ENTER_LUX "ad_night_lx"
#define NVS_KEY_DAY_EXIT_LUX    "ad_day_lx"
#define NVS_KEY_NIGHT_DIM_PCT   "ad_night_pct"
#define NVS_KEY_LUX_SAMPLES     "ad_lux_smpl"

#define DEFAULT_NIGHT_ENTER_LUX 5.0f
#define DEFAULT_DAY_EXIT_LUX    15.0f
#define DEFAULT_NIGHT_DIM_PCT   10
#define DEFAULT_LUX_SAMPLES     3

static auto_dim_config_t s_config = {
    .enabled = true,
    .night_enter_lux = DEFAULT_NIGHT_ENTER_LUX,
    .day_exit_lux = DEFAULT_DAY_EXIT_LUX,
    .night_dim_pct = DEFAULT_NIGHT_DIM_PCT,
    .lux_sample_count = DEFAULT_LUX_SAMPLES,
};

static bool s_is_night = false;
static int  s_effective_pct = 0;
static int  s_night_streak = 0;
static int  s_day_streak = 0;

static bool auto_dim_active(void)
{
    return aircube_model_is_pro() && s_config.enabled;
}

static bool save_config_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_u8(handle, NVS_KEY_AUTO_DIM_EN, s_config.enabled ? 1 : 0);
    nvs_set_u32(handle, NVS_KEY_NIGHT_ENTER_LUX, (uint32_t)(s_config.night_enter_lux * 100.0f));
    nvs_set_u32(handle, NVS_KEY_DAY_EXIT_LUX, (uint32_t)(s_config.day_exit_lux * 100.0f));
    nvs_set_i32(handle, NVS_KEY_NIGHT_DIM_PCT, s_config.night_dim_pct);
    nvs_set_i32(handle, NVS_KEY_LUX_SAMPLES, s_config.lux_sample_count);

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void load_config_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t enabled = 1;
    if (nvs_get_u8(handle, NVS_KEY_AUTO_DIM_EN, &enabled) == ESP_OK) {
        s_config.enabled = (enabled != 0);
    }

    uint32_t night_lx = 0;
    if (nvs_get_u32(handle, NVS_KEY_NIGHT_ENTER_LUX, &night_lx) == ESP_OK && night_lx > 0) {
        s_config.night_enter_lux = (float)night_lx / 100.0f;
    }

    uint32_t day_lx = 0;
    if (nvs_get_u32(handle, NVS_KEY_DAY_EXIT_LUX, &day_lx) == ESP_OK && day_lx > 0) {
        s_config.day_exit_lux = (float)day_lx / 100.0f;
    }

    int32_t night_pct = -1;
    if (nvs_get_i32(handle, NVS_KEY_NIGHT_DIM_PCT, &night_pct) == ESP_OK &&
        night_pct >= 0 && night_pct <= 100) {
        s_config.night_dim_pct = (int)night_pct;
    }

    int32_t samples = -1;
    if (nvs_get_i32(handle, NVS_KEY_LUX_SAMPLES, &samples) == ESP_OK &&
        samples >= 1 && samples <= 20) {
        s_config.lux_sample_count = (int)samples;
    }

    nvs_close(handle);
}

int auto_dim_preset_from_percent(int percent)
{
    if (percent <= 0)  return 0;
    if (percent <= 10) return 1;
    if (percent <= 30) return 2;
    if (percent <= 60) return 3;
    return 4;
}

static int effective_for_preset(int configured_pct, int preset, bool is_night)
{
    if (configured_pct <= 0) {
        return 0;
    }

    if (!is_night) {
        return configured_pct;
    }

    switch (preset) {
        case 0:
        case 1:
        case 2:
        case 3:
            return 0;
        case 4:
        default:
            return s_config.night_dim_pct;
    }
}

static int compute_effective_percent(int configured_pct)
{
    if (!auto_dim_active()) {
        return configured_pct;
    }

    int preset = auto_dim_preset_from_percent(configured_pct);
    return effective_for_preset(configured_pct, preset, s_is_night);
}

static void apply_effective_if_changed(int effective_pct)
{
    if (effective_pct < 0)   effective_pct = 0;
    if (effective_pct > 100) effective_pct = 100;

    if (effective_pct == s_effective_pct) {
        return;
    }

    s_effective_pct = effective_pct;
    led_set_intensity((float)effective_pct / 100.0f);
    ESP_LOGI(TAG, "Effective brightness %d%% (configured %d%%, night=%d)",
             s_effective_pct, button_get_brightness_percent(), s_is_night);
}

void auto_dim_init(void)
{
    load_config_from_nvs();

    // Migrate prior defaults to current thresholds (night < 5 lx, day > 15 lx).
    if ((s_config.night_enter_lux == 15.0f && s_config.day_exit_lux == 30.0f) ||
        (s_config.night_enter_lux == 12.0f && s_config.day_exit_lux == 20.0f) ||
        (s_config.night_enter_lux == 8.0f && s_config.day_exit_lux == 12.0f) ||
        (s_config.night_enter_lux == 4.0f && s_config.day_exit_lux == 10.0f) ||
        (s_config.night_enter_lux == 10.0f && s_config.day_exit_lux == 15.0f)) {
        s_config.night_enter_lux = DEFAULT_NIGHT_ENTER_LUX;
        s_config.day_exit_lux = DEFAULT_DAY_EXIT_LUX;
        save_config_to_nvs();
    }
    if (s_config.lux_sample_count == 1) {
        s_config.lux_sample_count = DEFAULT_LUX_SAMPLES;
        save_config_to_nvs();
    }

    if (!aircube_model_is_pro()) {
        s_config.enabled = false;
    }

    s_is_night = false;
    s_night_streak = 0;
    s_day_streak = 0;
    s_effective_pct = -1;

    ESP_LOGI(TAG, "Auto-dim %s (night<%.1f lux, day>%.1f lux, night_dim=%d%%)",
             s_config.enabled ? "enabled" : "disabled",
             s_config.night_enter_lux, s_config.day_exit_lux, s_config.night_dim_pct);

    auto_dim_recompute();
}

void auto_dim_update_lux(float lux)
{
    if (!auto_dim_active()) {
        return;
    }

    int samples = s_config.lux_sample_count;
    if (samples < 1) {
        samples = 1;
    }

    bool prev_night = s_is_night;

    if (lux < s_config.night_enter_lux) {
        s_night_streak++;
        s_day_streak = 0;
        if (s_night_streak >= samples) {
            s_is_night = true;
        }
    } else if (lux > s_config.day_exit_lux) {
        s_day_streak++;
        s_night_streak = 0;
        if (s_day_streak >= samples) {
            s_is_night = false;
        }
    } else {
        s_night_streak = 0;
        s_day_streak = 0;
    }

    if (prev_night != s_is_night) {
        ESP_LOGI(TAG, "Day/night transition -> %s (lux=%.1f)",
                 s_is_night ? "night" : "day", lux);
        auto_dim_recompute();
    }
}

void auto_dim_recompute(void)
{
    int configured = button_get_brightness_percent();
    int effective = compute_effective_percent(configured);
    apply_effective_if_changed(effective);
}

int auto_dim_get_effective_percent(void)
{
    if (s_effective_pct < 0) {
        return button_get_brightness_percent();
    }
    return s_effective_pct;
}

bool auto_dim_is_night(void)
{
    return s_is_night;
}

void auto_dim_get_config(auto_dim_config_t *config)
{
    if (config) {
        *config = s_config;
    }
}

void auto_dim_get_status(auto_dim_status_t *status)
{
    if (!status) {
        return;
    }

    status->config = s_config;
    status->is_night = s_is_night;
    status->configured_pct = button_get_brightness_percent();
    status->effective_pct = auto_dim_get_effective_percent();
    status->preset = auto_dim_preset_from_percent(status->configured_pct);
}

void auto_dim_set_config(const auto_dim_config_t *config)
{
    if (!config) {
        return;
    }

    s_config = *config;

    if (s_config.night_dim_pct < 0)   s_config.night_dim_pct = 0;
    if (s_config.night_dim_pct > 100) s_config.night_dim_pct = 100;
    if (s_config.lux_sample_count < 1)  s_config.lux_sample_count = 1;
    if (s_config.lux_sample_count > 20) s_config.lux_sample_count = 20;
    if (s_config.day_exit_lux <= s_config.night_enter_lux) {
        s_config.day_exit_lux = s_config.night_enter_lux + 5.0f;
    }

    if (!aircube_model_is_pro()) {
        s_config.enabled = false;
    }

    save_config_to_nvs();
    auto_dim_recompute();

    ESP_LOGI(TAG, "Config updated: enabled=%d night<%.1f day>%.1f night_dim=%d%%",
             s_config.enabled, s_config.night_enter_lux, s_config.day_exit_lux,
             s_config.night_dim_pct);
}
