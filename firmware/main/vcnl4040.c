//
// Driver for the Vishay VCNL4040 proximity + ambient light sensor.
//
// The VCNL4040 uses 16-bit registers addressed by a command byte. Each register
// transfers as two bytes, low byte first (little endian). Writes are
// [command, LSB, MSB]; reads write the command byte then read 2 bytes back.
//

#include "vcnl4040.h"
#include "i2c_driver.h"
#include "led.h"
#include "esp_log.h"

static const char *TAG = "vcnl4040";

#define VCNL4040_I2C_ADDRESS 0x60

// Register (command) map
#define VCNL4040_REG_ALS_CONF  0x00  // ALS integration time / persistence / enable
#define VCNL4040_REG_PS_CONF12 0x03  // PS_CONF1 (low) + PS_CONF2 (high)
#define VCNL4040_REG_PS_DATA   0x08  // proximity result
#define VCNL4040_REG_ALS_DATA  0x09  // ambient light result
#define VCNL4040_REG_DEVICE_ID 0x0C  // device ID, expected 0x0186

#define VCNL4040_DEVICE_ID 0x0186

// ALS_IT = 640 ms (ALS_CONF bits[7:6]=11) gives 0.0125 lux per count (finest resolution).
#define VCNL4040_LUX_PER_COUNT 0.0125f

// Calibration gain to compensate for enclosure-window attenuation. Measured against
// a reference lux meter: sensor read 40.24 lux while reference read 94 lux -> 94/40.24.
#define VCNL4040_LUX_CAL_GAIN 2.336f

// LED spill into the ALS at fixed brightness steps (lux above LED-off baseline).
// Measured with reference meter held constant: 0%=54, 25%=57, 50%=61, 75%=64, 100%=68 lx.
typedef struct {
    float pct;
    float spill_lux;
} lux_led_spill_point_t;

static const lux_led_spill_point_t LUX_LED_SPILL_TABLE[] = {
    {0.f, 0.f},
    {25.f, 3.f},
    {50.f, 7.f},
    {75.f, 10.f},
    {100.f, 14.f},
};

static float lux_led_spill_for_intensity_pct(float pct)
{
    const size_t count = sizeof(LUX_LED_SPILL_TABLE) / sizeof(LUX_LED_SPILL_TABLE[0]);

    if (pct <= LUX_LED_SPILL_TABLE[0].pct) {
        return LUX_LED_SPILL_TABLE[0].spill_lux;
    }
    if (pct >= LUX_LED_SPILL_TABLE[count - 1].pct) {
        return LUX_LED_SPILL_TABLE[count - 1].spill_lux;
    }

    for (size_t i = 0; i < count - 1; i++) {
        const lux_led_spill_point_t *lo = &LUX_LED_SPILL_TABLE[i];
        const lux_led_spill_point_t *hi = &LUX_LED_SPILL_TABLE[i + 1];
        if (pct >= lo->pct && pct <= hi->pct) {
            float span = hi->pct - lo->pct;
            float t = (pct - lo->pct) / span;
            return lo->spill_lux + t * (hi->spill_lux - lo->spill_lux);
        }
    }

    return 0.f;
}

static float lux_apply_led_compensation(float lux)
{
    if (led_get_color() == LED_COLOR_OFF) {
        return lux;
    }

    float spill = lux_led_spill_for_intensity_pct(led_get_intensity() * 100.f);
    lux -= spill;
    return (lux < 0.f) ? 0.f : lux;
}

static bool vcnl4040_present = false;

static uint16_t vcnl4040_proximity = 0;
static uint16_t vcnl4040_ambient = 0;

static esp_err_t vcnl4040_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (uint8_t)(value & 0xFF);        // low byte first
    buf[2] = (uint8_t)((value >> 8) & 0xFF); // high byte
    return i2c_driver_write(VCNL4040_I2C_ADDRESS, buf, 3);
}

static esp_err_t vcnl4040_read_reg(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};
    esp_err_t ret = i2c_driver_read(VCNL4040_I2C_ADDRESS, &reg, 1, data, 2);
    if (ret != ESP_OK) {
        return ret;
    }
    *value = (uint16_t)(data[0] | (data[1] << 8)); // little endian
    return ESP_OK;
}

bool vcnl4040_is_present(void)
{
    return vcnl4040_present;
}

uint16_t vcnl4040_get_proximity(void)   { return vcnl4040_proximity; }
uint16_t vcnl4040_get_ambient_raw(void) { return vcnl4040_ambient; }
float    vcnl4040_get_lux(void)
{
    float lux = (float)vcnl4040_ambient * VCNL4040_LUX_PER_COUNT * VCNL4040_LUX_CAL_GAIN;
    return lux_apply_led_compensation(lux);
}

void vcnl4040_init(void)
{
    vcnl4040_present = false;

    // Quietly check for an ACK at the VCNL4040 address first. On Base hardware
    // the sensor is absent by design, so a no-ACK is a normal "not present"
    // result rather than a fault (avoids noisy I2C error logs during detection).
    if (!i2c_driver_probe(VCNL4040_I2C_ADDRESS)) {
        ESP_LOGI(TAG, "VCNL4040 not present");
        return;
    }

    // Confirm identity via the device ID register.
    uint16_t id = 0;
    esp_err_t ret = vcnl4040_read_reg(VCNL4040_REG_DEVICE_ID, &id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VCNL4040 ID read failed: %s", esp_err_to_name(ret));
        return;
    }
    if (id != VCNL4040_DEVICE_ID) {
        ESP_LOGW(TAG, "VCNL4040 not detected (unexpected ID 0x%04X)", id);
        return;
    }

    // Enable ALS: ALS_CONF = 0x00C0 -> ALS_IT = 640 ms (bits[7:6]=11), ALS_SD = 0 (powered on).
    ret = vcnl4040_write_reg(VCNL4040_REG_ALS_CONF, 0x00C0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure ALS: %s", esp_err_to_name(ret));
        return;
    }

    // Enable PS: PS_CONF1/PS_CONF2 = 0x0000 -> PS_SD = 0 (powered on), defaults.
    ret = vcnl4040_write_reg(VCNL4040_REG_PS_CONF12, 0x0000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure PS: %s", esp_err_to_name(ret));
        return;
    }

    vcnl4040_present = true;
    ESP_LOGI(TAG, "VCNL4040 detected and initialized (ID 0x%04X)", id);
}

void vcnl4040_read(void)
{
    if (!vcnl4040_present) {
        return;
    }

    uint16_t value = 0;
    if (vcnl4040_read_reg(VCNL4040_REG_PS_DATA, &value) == ESP_OK) {
        vcnl4040_proximity = value;
    } else {
        ESP_LOGW(TAG, "Failed to read proximity data");
    }

    if (vcnl4040_read_reg(VCNL4040_REG_ALS_DATA, &value) == ESP_OK) {
        vcnl4040_ambient = value;
    } else {
        ESP_LOGW(TAG, "Failed to read ambient light data");
    }
}
