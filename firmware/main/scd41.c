//
// Driver for the Sensirion SCD41 CO2 / temperature / humidity sensor.
// Verified against the SCD4x datasheet v1.7 (April 2025).
//

#include "scd41.h"
#include "i2c_driver.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "scd41";

// 16-bit command words (most significant byte transmitted first)
#define SCD41_CMD_READ_MEASUREMENT        0xEC05
#define SCD41_CMD_STOP_PERIODIC           0x3F86
#define SCD41_CMD_GET_SERIAL_NUMBER       0x3682
#define SCD41_CMD_GET_SENSOR_VARIANT      0x202F
#define SCD41_CMD_MEASURE_SINGLE_SHOT     0x219D  // CO2 + RH + T, ~5000 ms exec
#define SCD41_CMD_MEASURE_SINGLE_SHOT_RHT 0x2196  // RH + T only, ~50 ms exec
#define SCD41_CMD_SET_TEMPERATURE_OFFSET  0x241D
#define SCD41_CMD_GET_DATA_READY          0xE4B8  // ~1 ms exec
#define SCD41_CMD_REINIT                  0x3646  // ~30 ms exec
#define SCD41_CMD_PERFORM_SELF_TEST       0x3639  // ~10000 ms exec

// CRC-8: polynomial 0x31, init 0xFF, no reflection, final XOR 0x00
#define SCD41_CRC8_POLYNOMIAL 0x31
#define SCD41_CRC8_INIT       0xFF

// Command execution times, in microseconds.
#define SCD41_EXEC_SHORT_US        1000       // datasheet "1 ms" commands
#define SCD41_EXEC_REINIT_US       30000
#define SCD41_EXEC_STOP_US         500000
#define SCD41_EXEC_SELF_TEST_US    10000000
#define SCD41_RHT_EXEC_US          50000      // measure_single_shot_rht_only
#define SCD41_CO2_EXEC_US          5000000    // measure_single_shot

// Single-shot cadence (wall-clock). RH+T is cheap (~50 ms) and runs often; the
// full CO2 measurement (~5 s, the main self-heating contributor) runs less
// often so the sensor stays closer to ambient temperature.
#define SCD41_RHT_INTERVAL_US   (5ULL  * 1000000ULL)   // RH+T every 5 s
#define SCD41_CO2_INTERVAL_US   (30ULL * 1000000ULL)   // CO2  every 30 s

// A triggered CO2 shot that has not produced data by this point is abandoned
// and counted as a failure, so a wedged sensor cannot park the state machine.
#define SCD41_CO2_DEADLINE_US   (8ULL * 1000000ULL)

// How long to keep asking whether a measurement is ready before giving up.
#define SCD41_READY_TIMEOUT_US       200000
#define SCD41_READY_POLL_INTERVAL_US 2000

// Above this, wait by sleeping; below it, busy-wait. Sleeping is preferable but
// cannot resolve short intervals (see scd41_delay_us).
#define SCD41_BUSY_WAIT_LIMIT_US 3000

// Values outside these bounds are not physically possible in a room and are
// treated as a bad read rather than data. Device 1's logs contain -44.47 C and
// 100.00 %RH samples, both of which these bounds reject.
#define SCD41_TEMP_MIN_C   (-10.0f)
#define SCD41_TEMP_MAX_C     60.0f
#define SCD41_RH_MIN_PCT      1.0f
#define SCD41_RH_MAX_PCT     99.0f
#define SCD41_CO2_MIN_PPM     1      // 0 ppm means "no measurement", not clean air
#define SCD41_CO2_MAX_PPM 40000      // sensor measurement range ceiling

// Consecutive bit-identical (temperature, humidity) word pairs that latch a
// stuck-channel fault. At the 5 s RH+T cadence this is about 5 minutes. A
// healthy sensor jitters by several LSBs between measurements (one LSB is
// 0.0027 C / 0.0015 %RH), so a run this long is not a quiet room.
#define SCD41_STUCK_THRESHOLD 60

// Readings older than this stop being reported as current.
#define SCD41_STALE_TIMEOUT_US     (60ULL  * 1000000ULL)
#define SCD41_CO2_STALE_TIMEOUT_US (180ULL * 1000000ULL)

// Failed reads before a reinit is attempted, and the ceiling on attempts. Once
// the ceiling is reached the fault is simply reported and no longer poked at.
// Attempts are spaced so a sensor that is never coming back is not reset in a
// tight loop; reads continue in between so genuine recovery is still noticed.
#define SCD41_RECOVERY_THRESHOLD    5
#define SCD41_RECOVERY_MAX_ATTEMPTS 10
#define SCD41_RECOVERY_SPACING_US   (60ULL * 1000000ULL)

// Retry pacing after a failure: doubles per consecutive failure up to 32 s, so
// a dead sensor is not hammered once per second forever.
#define SCD41_RETRY_BASE_US    (1ULL * 1000000ULL)
#define SCD41_RETRY_MAX_SHIFT  5

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

#define SCD41_MAX_WORDS 8

static bool scd41_present = false;

static uint16_t scd41_co2 = 0;
static float    scd41_temperature_c = 0.0f;
static float    scd41_humidity = 0.0f;

// Freshness bookkeeping. scd41_last_good_us and scd41_last_co2_good_us are the
// times of the last ACCEPTED values, as opposed to the last attempt.
static bool    scd41_ever_valid = false;
static bool    scd41_ever_co2_valid = false;
static int64_t scd41_last_good_us = 0;
static int64_t scd41_last_co2_good_us = 0;

// Stuck-channel detection.
static uint16_t scd41_prev_t_word = 0;
static uint16_t scd41_prev_rh_word = 0;
static bool     scd41_have_prev_words = false;
static uint32_t scd41_identical_streak = 0;
static bool     scd41_stuck_latched = false;
static uint32_t scd41_stuck_events = 0;

// Failure / recovery bookkeeping.
static uint32_t scd41_consecutive_failures = 0;
static uint32_t scd41_recovery_attempts = 0;
static uint32_t scd41_rejected_samples = 0;
static int64_t  scd41_retry_after_us = 0;
// Kept separate from scd41_retry_after_us so that a successful read (which a
// stuck sensor still produces) does not reset the recovery pacing.
static int64_t  scd41_recovery_after_us = 0;

// Serialises the multi-step command/delay/read sequences. scd41_poll() runs on
// the sensor task while the manual reinit and self test arrive on the command
// task, and neither sequence is safe to interleave with the other.
static SemaphoreHandle_t scd41_lock = NULL;

// Last read_measurement response, for the scd41_raw diagnostic.
static uint8_t  scd41_frame_bytes[9];
static uint16_t scd41_frame_words[3];
static uint8_t  scd41_frame_crc_mask = 0;
static int64_t  scd41_frame_us = 0;
static bool     scd41_frame_valid = false;

// Single-shot state machine. The full CO2 shot takes ~5 s, so it is issued and
// then read on a later poll() call rather than blocking.
typedef enum {
    SCD41_STATE_IDLE = 0,
    SCD41_STATE_WAIT_CO2,   // full single-shot issued; waiting to read the result
} scd41_state_t;

static scd41_state_t scd41_state       = SCD41_STATE_IDLE;
static int64_t       scd41_op_start_us = 0;   // when the async CO2 shot was issued
static int64_t       scd41_last_rht_us = 0;   // last RH+T attempt time
static int64_t       scd41_last_co2_us = 0;   // last CO2 attempt time

static esp_err_t scd41_recover(const char *reason);

// vTaskDelay() sleeps until a tick boundary, so a request of N ticks can elapse
// as little as N-1 ticks. At the default 100 Hz tick that turns a nominal 50 ms
// wait into 40 ms, and silently turns any request under 10 ms into no wait at
// all -- both of which make us read the sensor before it has answered. Short
// waits busy-wait; longer ones sleep with a whole extra tick of margin.
static void scd41_delay_us(uint32_t us)
{
    if (us == 0) {
        return;
    }
    if (us <= SCD41_BUSY_WAIT_LIMIT_US) {
        esp_rom_delay_us(us);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(us / 1000) + 1);
}

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
    scd41_delay_us(SCD41_EXEC_SHORT_US);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SCD41 temperature offset set to %.2f C", offset_c);
    } else {
        ESP_LOGW(TAG, "Failed to set temperature offset: %s", esp_err_to_name(ret));
    }
}

// Send a command, wait for the execution time, then read 'count' words.
// Each word in the response is 2 bytes followed by 1 CRC byte. The CRC of
// every word is verified; words[] receives the decoded 16-bit values.
static esp_err_t scd41_read_words(uint16_t command, uint16_t *words, int count, uint32_t delay_us)
{
    if (words == NULL || count <= 0 || count > SCD41_MAX_WORDS) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = scd41_send_command(command);
    if (ret != ESP_OK) {
        return ret;
    }

    scd41_delay_us(delay_us);

    // Zeroed so a driver that ever returns success without filling the buffer
    // yields an obvious 0x0000 with a failing CRC rather than stale stack data.
    uint8_t buf[3 * SCD41_MAX_WORDS];
    memset(buf, 0, sizeof(buf));

    ret = i2c_driver_read_raw(SCD41_I2C_ADDRESS, buf, count * 3);
    if (ret != ESP_OK) {
        return ret;
    }

    // CRC every word before acting on any of them, so the diagnostic capture
    // below records the whole frame even when one word is corrupt.
    uint8_t crc_mask = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *word = &buf[i * 3];
        if (scd41_crc8(word, 2) == word[2]) {
            crc_mask |= (uint8_t)(1u << i);
        }
    }

    if (command == SCD41_CMD_READ_MEASUREMENT && count == 3) {
        memcpy(scd41_frame_bytes, buf, sizeof(scd41_frame_bytes));
        for (int i = 0; i < 3; i++) {
            scd41_frame_words[i] = (uint16_t)((buf[i * 3] << 8) | buf[i * 3 + 1]);
        }
        scd41_frame_crc_mask = crc_mask;
        scd41_frame_us = esp_timer_get_time();
        scd41_frame_valid = true;
    }

    for (int i = 0; i < count; i++) {
        if (!(crc_mask & (1u << i))) {
            ESP_LOGW(TAG, "CRC mismatch on word %d (cmd 0x%04X)", i, command);
            return ESP_ERR_INVALID_CRC;
        }
        words[i] = (uint16_t)((buf[i * 3] << 8) | buf[i * 3 + 1]);
    }

    return ESP_OK;
}

// Ask the sensor whether a measurement is available. The low 11 bits are the
// meaningful part; all-zero means not ready.
static esp_err_t scd41_data_ready(bool *ready)
{
    uint16_t status = 0;
    esp_err_t ret = scd41_read_words(SCD41_CMD_GET_DATA_READY, &status, 1,
                                     SCD41_EXEC_SHORT_US);
    if (ret != ESP_OK) {
        return ret;
    }
    *ready = ((status & 0x07FF) != 0);
    return ESP_OK;
}

// Poll data-ready up to a bounded deadline. Replaces trusting a fixed delay.
static esp_err_t scd41_wait_data_ready(void)
{
    int64_t deadline = esp_timer_get_time() + SCD41_READY_TIMEOUT_US;
    for (;;) {
        bool ready = false;
        esp_err_t ret = scd41_data_ready(&ready);
        if (ret != ESP_OK) {
            return ret;
        }
        if (ready) {
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        scd41_delay_us(SCD41_READY_POLL_INTERVAL_US);
    }
}

static void scd41_note_success(void)
{
    scd41_consecutive_failures = 0;
    scd41_retry_after_us = 0;
}

// Count a failed read and push the next attempt out. Recovery is attempted from
// scd41_poll() rather than here, so it never runs from inside a read path.
static void scd41_note_failure(const char *what, esp_err_t err)
{
    if (scd41_consecutive_failures < UINT32_MAX) {
        scd41_consecutive_failures++;
    }

    uint32_t shift = scd41_consecutive_failures - 1;
    if (shift > SCD41_RETRY_MAX_SHIFT) {
        shift = SCD41_RETRY_MAX_SHIFT;
    }
    scd41_retry_after_us = esp_timer_get_time() + (int64_t)(SCD41_RETRY_BASE_US << shift);

    ESP_LOGW(TAG, "%s failed: %s (%lu consecutive, next attempt in %llu ms)",
             what, esp_err_to_name(err),
             (unsigned long)scd41_consecutive_failures,
             (SCD41_RETRY_BASE_US << shift) / 1000ULL);
}

// Track bit-identical (temperature, humidity) pairs. This is the signature the
// failed customer units showed: CO2 kept moving while both other words in the
// same frame stayed frozen for days. Latching clears itself the moment a
// different pair arrives, so a false positive costs one reading, not the unit.
static void scd41_check_stuck(uint16_t t_word, uint16_t rh_word)
{
    if (scd41_have_prev_words &&
        t_word == scd41_prev_t_word && rh_word == scd41_prev_rh_word) {
        scd41_identical_streak++;
        if (scd41_identical_streak >= SCD41_STUCK_THRESHOLD && !scd41_stuck_latched) {
            scd41_stuck_latched = true;
            scd41_stuck_events++;
            ESP_LOGE(TAG, "Temperature/humidity stuck: %lu identical readings of "
                          "T=0x%04X RH=0x%04X - treating as a sensor fault",
                     (unsigned long)scd41_identical_streak, t_word, rh_word);
        }
        return;
    }

    if (scd41_stuck_latched) {
        ESP_LOGW(TAG, "Temperature/humidity moving again (T=0x%04X RH=0x%04X) - "
                      "clearing stuck fault", t_word, rh_word);
        scd41_stuck_latched = false;
    }
    scd41_prev_t_word = t_word;
    scd41_prev_rh_word = rh_word;
    scd41_have_prev_words = true;
    scd41_identical_streak = 0;
}

// Convert raw temperature/humidity words to engineering units and accept them
// only if the result is physically possible.
// Temperature gets a software trim (see SCD41_TEMPERATURE_TRIM_C); humidity is
// passed through exactly as the sensor reports it.
static bool scd41_store_temp_rh(uint16_t t_word, uint16_t rh_word)
{
    float temperature = -45.0f + 175.0f * ((float)t_word / 65535.0f)
                        + SCD41_TEMPERATURE_TRIM_C;
    float humidity    = 100.0f * ((float)rh_word / 65535.0f);

    if (temperature < SCD41_TEMP_MIN_C || temperature > SCD41_TEMP_MAX_C ||
        humidity    < SCD41_RH_MIN_PCT || humidity    > SCD41_RH_MAX_PCT) {
        scd41_rejected_samples++;
        ESP_LOGW(TAG, "Implausible reading rejected: %.2f C, %.2f %%RH "
                      "(T=0x%04X RH=0x%04X)", temperature, humidity, t_word, rh_word);
        return false;
    }

    scd41_check_stuck(t_word, rh_word);

    scd41_temperature_c = temperature;
    scd41_humidity      = humidity;
    scd41_ever_valid    = true;
    scd41_last_good_us  = esp_timer_get_time();
    return true;
}

static bool scd41_store_co2(uint16_t co2_word)
{
    if (co2_word < SCD41_CO2_MIN_PPM || co2_word > SCD41_CO2_MAX_PPM) {
        scd41_rejected_samples++;
        ESP_LOGW(TAG, "Implausible CO2 rejected: %u ppm", co2_word);
        return false;
    }

    scd41_co2 = co2_word;
    scd41_ever_co2_valid = true;
    scd41_last_co2_good_us = esp_timer_get_time();
    return true;
}

bool scd41_is_present(void)
{
    return scd41_present;
}

uint16_t scd41_get_co2(void)            { return scd41_co2; }
float    scd41_get_temperature_c(void)  { return scd41_temperature_c; }
float    scd41_get_humidity(void)       { return scd41_humidity; }

bool scd41_has_data(void)
{
    if (!scd41_present || !scd41_ever_valid || scd41_stuck_latched) {
        return false;
    }
    return (esp_timer_get_time() - scd41_last_good_us) < (int64_t)SCD41_STALE_TIMEOUT_US;
}

bool scd41_has_co2(void)
{
    if (!scd41_present || !scd41_ever_co2_valid) {
        return false;
    }
    return (esp_timer_get_time() - scd41_last_co2_good_us) < (int64_t)SCD41_CO2_STALE_TIMEOUT_US;
}

void scd41_get_health(scd41_health_t *out)
{
    if (out == NULL) {
        return;
    }

    int64_t now = esp_timer_get_time();

    out->present              = scd41_present;
    out->data_valid           = scd41_has_data();
    out->co2_valid            = scd41_has_co2();
    out->stuck_latched        = scd41_stuck_latched;
    out->consecutive_failures = scd41_consecutive_failures;
    out->identical_streak     = scd41_identical_streak;
    out->stuck_events         = scd41_stuck_events;
    out->recovery_attempts    = scd41_recovery_attempts;
    out->rejected_samples     = scd41_rejected_samples;
    out->last_good_age_ms     = scd41_ever_valid
                                ? (now - scd41_last_good_us) / 1000 : -1;
    out->last_co2_age_ms      = scd41_ever_co2_valid
                                ? (now - scd41_last_co2_good_us) / 1000 : -1;
}

bool scd41_get_raw_frame(scd41_raw_frame_t *out)
{
    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->valid = scd41_frame_valid;
    if (!scd41_frame_valid) {
        out->age_ms = -1;
        return false;
    }

    memcpy(out->bytes, scd41_frame_bytes, sizeof(out->bytes));
    memcpy(out->words, scd41_frame_words, sizeof(out->words));
    out->crc_mask = scd41_frame_crc_mask;
    out->age_ms   = (esp_timer_get_time() - scd41_frame_us) / 1000;
    return true;
}

void scd41_init(void)
{
    if (scd41_lock == NULL) {
        scd41_lock = xSemaphoreCreateMutex();
        if (scd41_lock == NULL) {
            ESP_LOGE(TAG, "Cannot create SCD41 mutex - sensor disabled");
            return;
        }
    }

    scd41_present = false;
    scd41_ever_valid = false;
    scd41_ever_co2_valid = false;
    scd41_stuck_latched = false;
    scd41_have_prev_words = false;
    scd41_identical_streak = 0;
    scd41_consecutive_failures = 0;
    scd41_recovery_attempts = 0;
    scd41_rejected_samples = 0;
    scd41_retry_after_us = 0;
    scd41_recovery_after_us = 0;
    scd41_stuck_events = 0;
    scd41_frame_valid = false;

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
    scd41_delay_us(SCD41_EXEC_STOP_US);

    // Probe presence by reading the 48-bit serial number (3 CRC-checked words).
    uint16_t serial[3] = {0};
    esp_err_t ret = scd41_read_words(SCD41_CMD_GET_SERIAL_NUMBER, serial, 3,
                                     SCD41_EXEC_SHORT_US);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SCD41 not detected (serial read: %s)", esp_err_to_name(ret));
        return;
    }

    // Confirm the variant is specifically an SCD41 (bits[15:12] == 0b0001).
    uint16_t variant = 0;
    ret = scd41_read_words(SCD41_CMD_GET_SENSOR_VARIANT, &variant, 1,
                           SCD41_EXEC_SHORT_US);
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

// stop_periodic + reinit + re-apply offset. Deliberately does NOT issue
// perform_factory_reset: that erases the sensor's calibration, and a unit whose
// readings are already suspect should not also lose its calibration
// unattended.
static esp_err_t scd41_recover(const char *reason)
{
    scd41_recovery_attempts++;
    ESP_LOGW(TAG, "Recovery attempt %lu (%s): stop_periodic + reinit",
             (unsigned long)scd41_recovery_attempts, reason);

    scd41_send_command(SCD41_CMD_STOP_PERIODIC);
    scd41_delay_us(SCD41_EXEC_STOP_US);

    esp_err_t ret = scd41_send_command(SCD41_CMD_REINIT);
    scd41_delay_us(SCD41_EXEC_REINIT_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reinit command failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // reinit reloads user settings from EEPROM, so the offset must go back in.
    scd41_apply_temperature_offset(SCD41_TEMPERATURE_OFFSET_C);

    // Give the sensor a clean slate to be judged on. A latched stuck fault is
    // deliberately NOT cleared here: only genuinely moving readings clear it
    // (see scd41_check_stuck), so a reinit that did not actually fix anything
    // cannot put the old frozen value back into circulation.
    scd41_state = SCD41_STATE_IDLE;
    scd41_consecutive_failures = 0;
    scd41_retry_after_us = 0;
    scd41_have_prev_words = false;
    scd41_identical_streak = 0;

    int64_t now = esp_timer_get_time();
    scd41_last_rht_us = now - (int64_t)SCD41_RHT_INTERVAL_US;
    scd41_last_co2_us = now - (int64_t)SCD41_CO2_INTERVAL_US;

    ESP_LOGW(TAG, "Recovery complete; sensor will be re-assessed");
    return ESP_OK;
}

esp_err_t scd41_reinit(void)
{
    if (!scd41_present || scd41_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(scd41_lock, portMAX_DELAY);
    esp_err_t ret = scd41_recover("manual");
    // A manual reinit is an explicit request to re-assess the sensor, so it also
    // re-arms the automatic recovery budget.
    scd41_recovery_after_us = esp_timer_get_time() + (int64_t)SCD41_RECOVERY_SPACING_US;
    xSemaphoreGive(scd41_lock);
    return ret;
}

static esp_err_t scd41_self_test_locked(uint16_t *result)
{
    ESP_LOGW(TAG, "Running self test, this takes about 10 seconds");
    scd41_send_command(SCD41_CMD_STOP_PERIODIC);
    scd41_delay_us(SCD41_EXEC_STOP_US);

    esp_err_t ret = scd41_read_words(SCD41_CMD_PERFORM_SELF_TEST, result, 1,
                                     SCD41_EXEC_SELF_TEST_US);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Self test result 0x%04X (%s)", *result,
                 (*result == 0) ? "no malfunction" : "MALFUNCTION");
    } else {
        ESP_LOGE(TAG, "Self test failed: %s", esp_err_to_name(ret));
    }

    // The self test leaves the sensor idle; restart the cadence from here so we
    // do not immediately fire a measurement into a settling sensor.
    scd41_state = SCD41_STATE_IDLE;
    scd41_last_rht_us = esp_timer_get_time();
    scd41_last_co2_us = esp_timer_get_time();
    return ret;
}

esp_err_t scd41_self_test(uint16_t *result)
{
    if (!scd41_present || scd41_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(scd41_lock, portMAX_DELAY);
    esp_err_t ret = scd41_self_test_locked(result);
    xSemaphoreGive(scd41_lock);
    return ret;
}

// Read the measurement the sensor has waiting. Used by both single-shot paths.
static bool scd41_read_and_store(bool co2_is_fresh)
{
    uint16_t words[3] = {0};
    esp_err_t ret = scd41_read_words(SCD41_CMD_READ_MEASUREMENT, words, 3,
                                     SCD41_EXEC_SHORT_US);
    if (ret != ESP_OK) {
        scd41_note_failure("read_measurement", ret);
        return false;
    }

    // A frame that arrives intact but decodes to nonsense still counts as a
    // failed read, otherwise a sensor returning plausible-CRC garbage would
    // never trip the recovery path.
    bool stored_rht = scd41_store_temp_rh(words[1], words[2]);
    bool stored_co2 = co2_is_fresh ? scd41_store_co2(words[0]) : false;

    if (!stored_rht && !(co2_is_fresh && stored_co2)) {
        scd41_note_failure("measurement validation", ESP_ERR_INVALID_RESPONSE);
        return false;
    }

    scd41_note_success();
    return true;
}

static bool scd41_poll_locked(void)
{
    int64_t now = esp_timer_get_time();

    switch (scd41_state) {
    case SCD41_STATE_WAIT_CO2: {
        // A full single-shot is in progress; wait out its ~5 s execution time
        // (non-blocking across poll() calls), then retrieve CO2 + RH + T.
        int64_t elapsed = now - scd41_op_start_us;
        if (elapsed < (int64_t)SCD41_CO2_EXEC_US) {
            return false;
        }

        bool ready = false;
        esp_err_t ret = scd41_data_ready(&ready);
        if (ret != ESP_OK) {
            scd41_state = SCD41_STATE_IDLE;
            scd41_last_co2_us = now;
            scd41_note_failure("get_data_ready", ret);
            return false;
        }
        if (!ready) {
            // Keep waiting across polls, but never indefinitely.
            if (elapsed < (int64_t)SCD41_CO2_DEADLINE_US) {
                return false;
            }
            scd41_state = SCD41_STATE_IDLE;
            scd41_last_co2_us = now;
            scd41_note_failure("CO2 single-shot", ESP_ERR_TIMEOUT);
            return false;
        }

        scd41_state = SCD41_STATE_IDLE;
        scd41_last_co2_us = now;
        bool fresh = scd41_read_and_store(true);
        if (fresh) {
            // The CO2 shot also produced a fresh RH+T pair.
            scd41_last_rht_us = now;
        }
        return fresh;
    }

    case SCD41_STATE_IDLE:
    default:
        // Recovery and retry pacing only gate new work, never the collection of
        // a measurement already in flight (handled above).
        if ((scd41_consecutive_failures >= SCD41_RECOVERY_THRESHOLD || scd41_stuck_latched) &&
            scd41_recovery_attempts < SCD41_RECOVERY_MAX_ATTEMPTS &&
            now >= scd41_recovery_after_us) {
            scd41_recover(scd41_stuck_latched ? "stuck temperature/humidity"
                                              : "repeated read failures");
            scd41_recovery_after_us = esp_timer_get_time() + (int64_t)SCD41_RECOVERY_SPACING_US;
            return false;
        }
        // Past the attempt ceiling we stop reinitialising but keep reading, so a
        // sensor that comes back on its own still clears its own fault.
        if (now < scd41_retry_after_us) {
            return false;
        }

        // CO2 due: trigger a full single-shot now, read it on a later poll().
        if ((now - scd41_last_co2_us) >= (int64_t)SCD41_CO2_INTERVAL_US) {
            esp_err_t ret = scd41_send_command(SCD41_CMD_MEASURE_SINGLE_SHOT);
            if (ret == ESP_OK) {
                scd41_op_start_us = now;
                scd41_state = SCD41_STATE_WAIT_CO2;
                return false;
            }
            // Trigger failed. Record it, then fall through so the cheap RH+T
            // path still gets a turn instead of being starved by the CO2 path.
            scd41_last_co2_us = now;
            scd41_note_failure("trigger CO2 single-shot", ret);
            return false;
        }

        // RH+T due: cheap (~50 ms), so trigger and read within this call.
        if ((now - scd41_last_rht_us) >= (int64_t)SCD41_RHT_INTERVAL_US) {
            scd41_last_rht_us = now;   // advance regardless, so failures back off

            esp_err_t ret = scd41_send_command(SCD41_CMD_MEASURE_SINGLE_SHOT_RHT);
            if (ret != ESP_OK) {
                scd41_note_failure("trigger RH+T single-shot", ret);
                return false;
            }

            scd41_delay_us(SCD41_RHT_EXEC_US);

            ret = scd41_wait_data_ready();
            if (ret != ESP_OK) {
                scd41_note_failure("RH+T data-ready", ret);
                return false;
            }

            // RH+T-only: the CO2 word is not freshly measured; keep last CO2.
            return scd41_read_and_store(false);
        }
        return false;
    }
}

bool scd41_poll(void)
{
    if (!scd41_present || scd41_lock == NULL) {
        return false;
    }

    // Don't queue up behind a manual self test; just skip this tick.
    if (xSemaphoreTake(scd41_lock, 0) != pdTRUE) {
        return false;
    }
    bool fresh = scd41_poll_locked();
    xSemaphoreGive(scd41_lock);
    return fresh;
}
