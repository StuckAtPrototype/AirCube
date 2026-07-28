#include "device_model.h"

#include "scd41.h"
#include "vcnl4040.h"
#include "esp_log.h"

static const char *TAG = "device_model";

static aircube_model_t s_model = AIRCUBE_MODEL_BASE;

void aircube_model_detect(void)
{
    bool scd41   = scd41_is_present();
    bool vcnl4040 = vcnl4040_is_present();

    // The SCD41/VCNL4040 pair only exists on Pro hardware; either one being
    // present means we are running on a Pro unit.
    if (scd41 || vcnl4040) {
        s_model = AIRCUBE_MODEL_PRO;
        if (!(scd41 && vcnl4040)) {
            ESP_LOGW(TAG,
                     "Pro hardware detected but sensor set is incomplete "
                     "(SCD41=%d, VCNL4040=%d) - check wiring",
                     scd41, vcnl4040);
        }
    } else {
        s_model = AIRCUBE_MODEL_BASE;
    }

    ESP_LOGI(TAG, "Detected hardware model: %s (SCD41=%d, VCNL4040=%d)",
             aircube_model_name(), scd41, vcnl4040);
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
