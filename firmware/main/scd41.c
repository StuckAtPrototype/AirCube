//
// Driver for the Sensirion SCD41 CO2 / temperature / humidity sensor.
// Verified against the SCD4x datasheet v1.7 (April 2025).
//

#include "scd41.h"
#include "i2c_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "scd41";

#define SCD41_I2C_ADDRESS 0x62

// 16-bit command words (most significant byte transmitted first)
#define SCD41_CMD_READ_MEASUREMENT        0xEC05
#define SCD41_CMD_STOP_PERIODIC           0x3F86
#define SCD41_CMD_GET_SERIAL_NUMBER       0x3682
#define SCD41_CMD_GET_SENSOR_VARIANT      0x202F
#define SCD41_CMD_MEASURE_SINGLE_SHOT     0x219D  // CO2 + RH + T, ~5000 ms exec
#define SCD41_CMD_MEASURE_SINGLE_SHOT_RHT 0x2196  // RH + T only, ~50 ms exec
#define SCD41_CMD_SET_TEMPERATURE_OFFSET  0x241D

// CRC-8: polynomial 0x31, init 0xFF, no reflection, final XOR 0x00
#define SCD41_CRC8_POLYNOMIAL 0x31
#define SCD41_CRC8_INIT       0xFF

// Single-shot cadence (wall-clock). RH+T is cheap (~50 ms) and runs often; the
// full CO2 measurement (~5 s, the main self-heating contributor) runs less
// often so the sensor stays closer to ambient temperature.
#define SCD41_RHT_INTERVAL_US   (5ULL  * 1000000ULL)   // RH+T every 5 s
#define SCD41_CO2_INTERVAL_US   (30ULL * 1000000ULL)   // CO2  every 30 s
#define SCD41_CO2_EXEC_MS       5000                   // measure_single_shot
#define SCD41_RHT_EXEC_MS       50                     // measure_single_shot_rht_only

// On-chip temperature offset (deg C). The SCD4x subtracts this from the raw
// reading and ALSO uses it to compensate the reported humidity. We deliberately
// keep it at the 4.0 C factory default because at that value the SCD41 humidity
// matches reality (the ENS210 reads ~12 %RH too low). Changing this would shift
// humidity as a side effect, so temperature is corrected separately in software
// via SCD41_TEMPERATURE_TRIM_C below.
#define SCD41_TEMPERATURE_OFFSET_C  4.0f

// Software-only temperature trim (deg C), added to the reported temperature
// after the sensor's own compensation. Humidity is NOT touched. Trimmed against
// a calibrated reference thermometer: the SCD41 read 79.8 F while the reference
// showed 77.4 F (2.4 F = 1.33 C too high), so the previous +1.86 C trim is
// reduced by that amount to 0.53 C.
#define SCD41_TEMPERATURE_TRIM_C    0.53f

static bool scd41_present = false;
static bool scd41_data_valid = false;   // true after first valid T/RH measurement

static uint16_t scd41_co2 = 0;
static float scd41_temperature_c = 0.0f;
static float scd41_humidity = 0.0f;

// Single-shot state machine. The full CO2 shot takes ~5 s, so it is issued and
// then read on a later poll() call rather than blocking.
typedef enum {
    SCD41_STATE_IDLE = 0,
    SCD41_STATE_WAIT_CO2,   // full single-shot issued; waiting to read the result
} scd41_state_t;

static scd41_state_t scd41_state    = SCD41_STATE_IDLE;
static int64_t       scd41_op_start_us = 0;   // when the async CO2 shot was issued
static int64_t       scd41_last_rht_us = 0;   // last RH+T sample time
static int64_t       scd41_last_co2_us = 0;   // last CO2 sample time

static uint8_t scd41_crc8(const uint8_t *data, int len)
{
    uint8_t crc = SCD41_CRC8_INIT;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ SCD41_CRC8_POLYNOMIAL);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

// Send a bare 16-bit command word (no parameters).
static esp_err_t scd41_send_command(uint16_t command)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(command >> 8);
    buf[1] = (uint8_t)(command & 0xFF);
    return i2c_driver_write(SCD41_I2C_ADDRESS, buf, 2);
}

// Send a 16-bit command word followed by a 16-bit argument and its CRC.
static esp_err_t scd41_send_command_arg(uint16_t command, uint16_t arg)
{
    uint8_t buf[5];
    buf[0] = (uint8_t)(command >> 8);
    buf[1] = (uint8_t)(command & 0xFF);
    buf[2] = (uint8_t)(arg >> 8);
    buf[3] = (uint8_t)(arg & 0xFF);
    buf[4] = scd41_crc8(&buf[2], 2);
    return i2c_driver_write(SCD41_I2C_ADDRESS, buf, 5);
}

// Apply the static temperature offset (deg C). Must be called while the sensor
// is idle. Not persisted to EEPROM - re-applied on every boot to avoid wear.
// The offset word is offset_c * 65535 / 175 per the SCD4x datasheet.
static void scd41_apply_temperature_offset(float offset_c)
{
    uint16_t word = (uint16_t)((offset_c * 65535.0f) / 175.0f + 0.5f);
    esp_err_t ret = scd41_send_command_arg(SCD41_CMD_SET_TEMPERATURE_OFFSET, word);
    vTaskDelay(pdMS_TO_TICKS(1));   // set_temperature_offset execution time
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SCD41 temperature offset set to %.2f C", offset_c);
    } else {
        ESP_LOGW(TAG, "Failed to set temperature offset: %s", esp_err_to_name(ret));
    }
}

// Send a command, wait for the execution time, then read 'count' words.
// Each word in the response is 2 bytes followed by 1 CRC byte. The CRC of
// every word is verified; words[] receives the decoded 16-bit values.
static esp_err_t scd41_read_words(uint16_t command, uint16_t *words, int count, uint32_t delay_ms)
{
    esp_err_t ret = scd41_send_command(command);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    uint8_t buf[3 * 8]; // up to 8 words supported here
    int bytes = count * 3;
    ret = i2c_driver_read_raw(SCD41_I2C_ADDRESS, buf, bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    for (int i = 0; i < count; i++) {
        const uint8_t *word = &buf[i * 3];
        if (scd41_crc8(word, 2) != word[2]) {
            ESP_LOGW(TAG, "CRC mismatch on word %d (cmd 0x%04X)", i, command);
            return ESP_ERR_INVALID_CRC;
        }
        words[i] = (uint16_t)((word[0] << 8) | word[1]);
    }

    return ESP_OK;
}

bool scd41_is_present(void)
{
    return scd41_present;
}

uint16_t scd41_get_co2(void)            { return scd41_co2; }
float    scd41_get_temperature_c(void)  { return scd41_temperature_c; }
float    scd41_get_humidity(void)       { return scd41_humidity; }
bool     scd41_has_data(void)           { return scd41_data_valid; }

void scd41_init(void)
{
    scd41_present = false;
    scd41_data_valid = false;

    // Quietly check whether anything is on the bus at the SCD41 address first.
    // On Base hardware the sensor is absent by design, so treat a no-ACK as a
    // normal "not present" result instead of letting the probe transactions
    // emit alarming I2C error logs.
    if (!i2c_driver_probe(SCD41_I2C_ADDRESS)) {
        ESP_LOGI(TAG, "SCD41 not present");
        return;
    }

    // The sensor only responds 500 ms after stop_periodic_measurement, and the
    // command itself is only valid from idle/periodic state. Send it first to
    // guarantee a known idle state, then wait the required settling time.
    scd41_send_command(SCD41_CMD_STOP_PERIODIC);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Probe presence by reading the 48-bit serial number (3 CRC-checked words).
    uint16_t serial[3] = {0};
    esp_err_t ret = scd41_read_words(SCD41_CMD_GET_SERIAL_NUMBER, serial, 3, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SCD41 not detected (serial read: %s)", esp_err_to_name(ret));
        return;
    }

    // Confirm the variant is specifically an SCD41 (bits[15:12] == 0b0001).
    uint16_t variant = 0;
    ret = scd41_read_words(SCD41_CMD_GET_SENSOR_VARIANT, &variant, 1, 1);
    if (ret == ESP_OK) {
        uint8_t variant_code = (variant >> 12) & 0x0F;
        const char *variant_str = (variant_code == 0x0) ? "SCD40" :
                                  (variant_code == 0x1) ? "SCD41" :
                                  (variant_code == 0x5) ? "SCD43" : "unknown";
        ESP_LOGI(TAG, "SCD4x variant: %s (0x%04X)", variant_str, variant);
    }

    ESP_LOGI(TAG, "SCD41 detected, serial: %04X%04X%04X", serial[0], serial[1], serial[2]);

    // Apply the static self-heating temperature offset while idle, then leave
    // the sensor idle so scd41_poll() can drive single-shot measurements.
    scd41_apply_temperature_offset(SCD41_TEMPERATURE_OFFSET_C);

    // Seed the cadence timers "one full interval ago" so the first poll()
    // triggers a CO2 single-shot immediately (first reading ~5 s after boot).
    scd41_state       = SCD41_STATE_IDLE;
    scd41_op_start_us = 0;
    scd41_last_rht_us = -(int64_t)SCD41_RHT_INTERVAL_US;
    scd41_last_co2_us = -(int64_t)SCD41_CO2_INTERVAL_US;

    scd41_present = true;
    ESP_LOGI(TAG, "SCD41 initialized in single-shot mode "
                  "(RH+T every %llu s, CO2 every %llu s)",
             SCD41_RHT_INTERVAL_US / 1000000ULL,
             SCD41_CO2_INTERVAL_US / 1000000ULL);
}

// Convert raw temperature/humidity words to engineering units.
// Temperature gets a software trim (see SCD41_TEMPERATURE_TRIM_C); humidity is
// passed through exactly as the sensor reports it.
static void scd41_store_temp_rh(uint16_t t_word, uint16_t rh_word)
{
    scd41_temperature_c = -45.0f + 175.0f * ((float)t_word / 65535.0f)
                          + SCD41_TEMPERATURE_TRIM_C;
    scd41_humidity      = 100.0f * ((float)rh_word / 65535.0f);
    scd41_data_valid    = true;
}

bool scd41_poll(void)
{
    if (!scd41_present) {
        return false;
    }

    int64_t now = esp_timer_get_time();

    switch (scd41_state) {
    case SCD41_STATE_WAIT_CO2: {
        // A full single-shot is in progress; wait out its ~5 s execution time
        // (non-blocking across poll() calls), then retrieve CO2 + RH + T.
        if ((now - scd41_op_start_us) < (int64_t)SCD41_CO2_EXEC_MS * 1000) {
            return false;
        }
        uint16_t words[3] = {0};
        esp_err_t ret = scd41_read_words(SCD41_CMD_READ_MEASUREMENT, words, 3, 1);
        scd41_state = SCD41_STATE_IDLE;
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "CO2 single-shot read failed: %s", esp_err_to_name(ret));
            return false;
        }
        scd41_co2 = words[0];
        scd41_store_temp_rh(words[1], words[2]);
        scd41_last_co2_us = now;
        scd41_last_rht_us = now;   // the CO2 shot also refreshed RH+T
        return true;
    }

    case SCD41_STATE_IDLE:
    default:
        // CO2 due: trigger a full single-shot now, read it on a later poll().
        if ((now - scd41_last_co2_us) >= (int64_t)SCD41_CO2_INTERVAL_US) {
            if (scd41_send_command(SCD41_CMD_MEASURE_SINGLE_SHOT) == ESP_OK) {
                scd41_op_start_us = now;
                scd41_state = SCD41_STATE_WAIT_CO2;
            } else {
                ESP_LOGW(TAG, "Failed to trigger CO2 single-shot");
            }
            return false;
        }
        // RH+T due: cheap (~50 ms), so trigger and read within this call.
        if ((now - scd41_last_rht_us) >= (int64_t)SCD41_RHT_INTERVAL_US) {
            if (scd41_send_command(SCD41_CMD_MEASURE_SINGLE_SHOT_RHT) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to trigger RH+T single-shot");
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(SCD41_RHT_EXEC_MS));
            uint16_t words[3] = {0};
            esp_err_t ret = scd41_read_words(SCD41_CMD_READ_MEASUREMENT, words, 3, 1);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "RH+T single-shot read failed: %s", esp_err_to_name(ret));
                return false;
            }
            // RH+T-only: the CO2 word is not freshly measured; keep last CO2.
            scd41_store_temp_rh(words[1], words[2]);
            scd41_last_rht_us = now;
            return true;
        }
        return false;
    }
}
