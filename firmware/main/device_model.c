#include "device_model.h"

#include "i2c_driver.h"
#include "scd41.h"
#include "vcnl4040.h"
#include "ens210.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "device_model";

#define NVS_NAMESPACE "aircube"
#define NVS_KEY_MODEL "model"

static aircube_model_t        s_model  = AIRCUBE_MODEL_BASE;
static aircube_model_source_t s_source = AIRCUBE_MODEL_SOURCE_DEFAULT;
static aircube_model_probe_t  s_probe;
static bool                   s_sensor_missing = false;

static bool nvs_read_model(aircube_model_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t val = 0;
    bool found = (nvs_get_u8(h, NVS_KEY_MODEL, &val) == ESP_OK);
    nvs_close(h);

    if (!found) {
        return false;
    }
    // Anything other than the two known encodings means the latch is corrupt;
    // fall through to a fresh probe rather than trusting it.
    if (val != AIRCUBE_MODEL_BASE && val != AIRCUBE_MODEL_PRO) {
        ESP_LOGW(TAG, "Latched model value %u is not valid - re-probing", val);
        return false;
    }
    *out = (aircube_model_t)val;
    return true;
}

static void nvs_write_model(aircube_model_t model)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS to latch model: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(h, NVS_KEY_MODEL, (uint8_t)model);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot latch model: %s", esp_err_to_name(err));
    }
}

static void probe_bus(void)
{
    s_probe.scd41    = i2c_driver_probe(SCD41_I2C_ADDRESS);
    s_probe.vcnl4040 = i2c_driver_probe(VCNL4040_I2C_ADDRESS);
    s_probe.ens210   = i2c_driver_probe(ENS210_I2C_ADDRESS);

    ESP_LOGI(TAG, "Bus probe: SCD41=%d VCNL4040=%d ENS210=%d",
             s_probe.scd41, s_probe.vcnl4040, s_probe.ens210);
}

// Deliberately stricter than "either Pro sensor answered". The answer gets
// latched, so one flaky probe on a factory-fresh unit would otherwise stick.
static aircube_model_t resolve_from_probe(void)
{
    if (s_probe.scd41) {
        return AIRCUBE_MODEL_PRO;
    }
    if (s_probe.vcnl4040 && !s_probe.ens210) {
        return AIRCUBE_MODEL_PRO;
    }
    return AIRCUBE_MODEL_BASE;
}

// Compare the probe against what the resolved model requires. A shortfall is
// reported as a fault and never changes s_model.
static void check_expected_sensors(void)
{
    s_sensor_missing = false;

    if (s_model == AIRCUBE_MODEL_PRO) {
        if (!s_probe.scd41) {
            ESP_LOGE(TAG, "Pro unit but SCD41 did not ACK - CO2, temperature "
                          "and humidity are unavailable (sensor fault)");
            s_sensor_missing = true;
        }
        if (!s_probe.vcnl4040) {
            ESP_LOGE(TAG, "Pro unit but VCNL4040 did not ACK - ambient light "
                          "and auto-dim are unavailable (sensor fault)");
            s_sensor_missing = true;
        }
    } else {
        if (!s_probe.ens210) {
            ESP_LOGE(TAG, "Base unit but ENS210 did not ACK - temperature and "
                          "humidity are unavailable (sensor fault)");
            s_sensor_missing = true;
        }
    }
}

void aircube_model_resolve(void)
{
    probe_bus();

    aircube_model_t stored;
    if (nvs_read_model(&stored)) {
        s_model  = stored;
        s_source = AIRCUBE_MODEL_SOURCE_NVS;
        ESP_LOGI(TAG, "Model %s (latched in NVS)", aircube_model_name());
    } else {
        s_model  = resolve_from_probe();
        s_source = AIRCUBE_MODEL_SOURCE_PROBE;
        nvs_write_model(s_model);
        ESP_LOGW(TAG, "Model %s resolved from bus probe and latched in NVS",
                 aircube_model_name());
    }

    check_expected_sensors();
}

aircube_model_t aircube_model_get(void)
{
    return s_model;
}

bool aircube_model_is_pro(void)
{
    return s_model == AIRCUBE_MODEL_PRO;
}

const char *aircube_model_name(void)
{
    return s_model == AIRCUBE_MODEL_PRO ? "pro" : "base";
}

aircube_model_source_t aircube_model_source(void)
{
    return s_source;
}

const char *aircube_model_source_name(void)
{
    switch (s_source) {
        case AIRCUBE_MODEL_SOURCE_NVS:   return "nvs";
        case AIRCUBE_MODEL_SOURCE_PROBE: return "probe";
        default:                         return "default";
    }
}

aircube_model_probe_t aircube_model_probe_result(void)
{
    return s_probe;
}

bool aircube_model_sensor_missing(void)
{
    return s_sensor_missing;
}

esp_err_t aircube_model_force_redetect(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_MODEL);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   // nothing latched yet is not a failure
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot clear model latch: %s", esp_err_to_name(err));
        return err;
    }

    aircube_model_t previous = s_model;
    aircube_model_resolve();
    ESP_LOGW(TAG, "Model re-detected: %s -> %s. Reboot so the correct sensors "
                  "are initialised.",
             previous == AIRCUBE_MODEL_PRO ? "pro" : "base", aircube_model_name());
    return ESP_OK;
}
