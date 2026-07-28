#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "led_color_lib.h"

#include "led.h"
#include "ens210.h"
#include "ens16x_driver.h"
#include "scd41.h"
#include "vcnl4040.h"
#include "device_model.h"
#include "i2c_driver.h"
#include "serial_protocol.h"
#include "button.h"
#include "auto_dim.h"
#include "history.h"
#include "zigbee.h"
#include "radio_mode.h"
#include "ble_gatt.h"

static const char *TAG = "main";

#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY 5
#define COMMAND_TASK_STACK_SIZE 4096
#define COMMAND_TASK_PRIORITY 4

// Configurable sensor readout period (default 1000ms)
static uint32_t sensor_readout_period_ms = 1000;
static SemaphoreHandle_t readout_period_mutex = NULL;

// LED color mapping: continuous green->red gradient from whichever of
// VOC Level / CO2 Level is currently worse (Pro); VOC Level only (Base).
#define AQI_MIN 0
#define AQI_MAX 200
#define AQI_GREEN_THRESHOLD 10  // Values 0-10 are pure green

#define HUE_GREEN  21845  // 120 deg - pure green

// Shared 0-500 severity checkpoints. Both the VOC and CO2 threshold tables
// below map their raw units onto these same bands, so the two resulting
// levels are directly comparable (see co2_calculate()/aqi_calculate()).
static const uint16_t AQI_LEVEL_BANDS[6] = {
    0,    // Level 0
    15,   // Level 1 - Excellent / Good edge
    50,   // Level 2 - Good / Moderate edge
    100,  // Level 3 - Moderate / Poor edge
    200,  // Level 4 - Poor / Unhealthy edge
    500   // Level 5 - Unhealthy cap
};

// VOC Level values at each TVOC band edge (matches published VOC table):
//   0-65 ppb      -> VOC Level 0-15   (Excellent)
//   65-220 ppb    -> VOC Level 15-50  (Good)
//   220-650 ppb   -> VOC Level 50-100 (Moderate)
//   650-2200 ppb  -> VOC Level 100-200 (Poor)
//   2200-5500 ppb -> VOC Level 200-500 (Unhealthy)
// Driven by TVOC alone; eCO2 is reported separately as raw ppm.
static const int AQI_TVOC_THRESHOLDS_PPB[5] = { 65, 220, 650, 2200, 5500 };

// CO2 ppm bands -> CO2 Level (EN 16798-1-style indoor-air categories),
// Pro hardware (SCD41) only:
//   800-1000 ppm   -> CO2 Level 15-50   (Cat I/II edge)
//   1000-1500 ppm  -> CO2 Level 50-100  (Cat II/III edge)
//   1500-2000 ppm  -> CO2 Level 100-200 (Cat III/IV edge)
//   2000-5000 ppm  -> CO2 Level 200-500 (unhealthy -> OSHA 8h TWA cap)
static const int AQI_CO2_THRESHOLDS_PPM[5] = { 800, 1000, 1500, 2000, 5000 };

// Canonical VOC Level (TVOC-derived) and CO2 Level (SCD41-derived, Pro only)
// for LED color mapping. The LED loop picks whichever is worse each tick.
static int current_voc_level = 0;
static int current_co2_level = 0;  // Pro only; 0 (best) until SCD41 has data

// Static variables for smooth LED color transitions
static float current_hue = 21845.0f;  // Current hue value (21845 = green, 0 = red) - using float for smooth transitions
static uint16_t target_hue = 21845;   // Target hue value we want to transition to
#define TRANSITION_SPEED 0.02f  // Transition speed per update (0.0 to 1.0, higher = faster)
// With 20ms update interval and 0.02 speed, full transition takes ~1 second (50 steps)

// Getter and setter for sensor readout period (for serial_protocol.c)
uint32_t get_sensor_readout_period_ms(void)
{
    uint32_t period = 1000;
    if (readout_period_mutex != NULL) {
        if (xSemaphoreTake(readout_period_mutex, portMAX_DELAY) == pdTRUE) {
            period = sensor_readout_period_ms;
            xSemaphoreGive(readout_period_mutex);
        }
    }
    return period;
}

void set_sensor_readout_period_ms(uint32_t period)
{
    if (readout_period_mutex != NULL) {
        if (xSemaphoreTake(readout_period_mutex, portMAX_DELAY) == pdTRUE) {
            sensor_readout_period_ms = period;
            xSemaphoreGive(readout_period_mutex);
        }
    }
}

/**
 * @brief Map VOC Level value to hue
 * 
 * Maps canonical VOC Level (TVOC-derived, 0-500) to LED hue:
 * - VOC Level 0-10: pure green (no color change)
 * - VOC Level 10-200: smooth gradient from green to red
 * - VOC Level 200+: full red (handled by caller)
 * 
 * @param aqi VOC Level value
 * @return 16-bit hue value (21845 = green, 0 = red)
 */
static uint16_t aqi_to_hue(int aqi)
{
    // Clamp VOC Level to valid range
    if (aqi < AQI_MIN) aqi = AQI_MIN;
    if (aqi > AQI_MAX) aqi = AQI_MAX;
    
    // Values 0-10 are pure green
    if (aqi <= AQI_GREEN_THRESHOLD) {
        return HUE_GREEN;
    }
    
    // For values 10-200, map smoothly from green to red
    // Map VOC Level from [10, 200] to ratio [0.0, 1.0] for smooth gradient
    // When VOC Level = 10: ratio = 0.0 (green)
    // When VOC Level = 200: ratio = 1.0 (red)
    float ratio = (float)(aqi - AQI_GREEN_THRESHOLD) / (float)(AQI_MAX - AQI_GREEN_THRESHOLD);
    
    // Linear interpolation from green to red
    // ratio = 0.0 -> hue = HUE_GREEN (green)
    // ratio = 1.0 -> hue = 0 (red)
    uint16_t hue = HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
    
    return hue;
}

/**
 * @brief Map a raw sensor value to a continuous band position 0.0..5.0
 *
 * Generic across sources: interpolates `value` against a 5-point threshold
 * table (ppb for VOC, ppm for CO2) whose edges correspond to band 1..5.
 */
static float value_to_level_pos(int value, const int thresholds[5])
{
    if (value <= 0) {
        return 0.0f;
    }
    if (value >= thresholds[4]) {
        return 5.0f;
    }
    if (value < thresholds[0]) {
        return (float)value / (float)thresholds[0];
    }
    for (int i = 0; i < 4; i++) {
        if (value < thresholds[i + 1]) {
            float span = (float)(thresholds[i + 1] - thresholds[i]);
            float frac = (float)(value - thresholds[i]) / span;
            return (float)(i + 1) + frac;
        }
    }
    return 5.0f;
}

/**
 * @brief Map a raw sensor value to a 0-500 severity level via a threshold table.
 *
 * Shared by aqi_calculate() (VOC, TVOC ppb) and co2_calculate() (CO2 ppm) so
 * both land on the same 0-500 scale and are directly comparable.
 *
 * @param value Raw sensor value (ppb or ppm)
 * @param thresholds 5-point band-edge table, in the same units as `value`
 * @param bands 6-point Level table (see AQI_LEVEL_BANDS)
 * @return Severity level in [0, 500]
 */
static uint16_t level_from_thresholds(int value, const int thresholds[5], const uint16_t bands[6])
{
    float pos = value_to_level_pos(value, thresholds);
    if (pos <= 0.0f) {
        return bands[0];
    }
    if (pos >= 5.0f) {
        return bands[5];
    }

    int low = (int)pos;
    if (low > 4) {
        low = 4;
    }
    float frac = pos - (float)low;

    int32_t a_low  = (int32_t)bands[low];
    int32_t a_high = (int32_t)bands[low + 1];
    int32_t level = a_low + (int32_t)((a_high - a_low) * frac);

    if (level < 0) {
        level = 0;
    }
    if (level > 500) {
        level = 500;
    }
    return (uint16_t)level;
}

/**
 * @brief Compute the canonical AirCube VOC Level from TVOC.
 *
 * Maps TVOC ppb to VOC Level 0-500 using fixed indoor-air bands (see AQI_LEVEL_BANDS).
 * TVOC-only; eCO2 is reported separately. LED color follows this VOC Level via
 * aqi_to_hue() (green at 0-10, gradient to red at 200+).
 *
 * @param etvoc Equivalent TVOC in ppb
 * @return VOC Level value in [0, 500]
 */
static uint16_t aqi_calculate(int etvoc)
{
    return level_from_thresholds(etvoc, AQI_TVOC_THRESHOLDS_PPB, AQI_LEVEL_BANDS);
}

/**
 * @brief Compute the AirCube CO2 Level from true (SCD41) CO2 ppm. Pro only.
 *
 * Maps CO2 ppm to CO2 Level 0-500 using EN 16798-1-style bands (see
 * AQI_CO2_THRESHOLDS_PPM), on the same 0-500 scale as VOC Level so the LED
 * loop can directly compare the two and show whichever is worse.
 *
 * @param co2_ppm True CO2 in ppm
 * @return CO2 Level value in [0, 500]
 */
static uint16_t co2_calculate(int co2_ppm)
{
    return level_from_thresholds(co2_ppm, AQI_CO2_THRESHOLDS_PPM, AQI_LEVEL_BANDS);
}

/**
 * @brief Startup animation - sweeps from green to red and back to green
 * 
 * This function displays a 3-second animation that smoothly transitions
 * from green to red and back to green when the device starts up.
 * Uses time-based animation to ensure accurate timing.
 */
static void startup_animation(void) {
    const uint32_t ANIMATION_DURATION_MS = 3000;  // 3 seconds total
    const uint32_t UPDATE_INTERVAL_MS = 10;  // Update every 10ms for smooth animation
    
    // Get start time in ticks
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(ANIMATION_DURATION_MS);
    
    while (1) {
        // Calculate elapsed time
        TickType_t current_ticks = xTaskGetTickCount();
        TickType_t elapsed_ticks = current_ticks - start_ticks;
        
        // Check if animation is complete
        if (elapsed_ticks >= duration_ticks) {
            break;
        }
        
        // Calculate progress from 0.0 to 1.0 based on elapsed time
        float progress = (float)elapsed_ticks / (float)duration_ticks;
        
        uint16_t hue;
        if (progress <= 0.5f) {
            // First half: green to red (0.0 to 0.5)
            float ratio = progress * 2.0f;  // 0.0 to 1.0
            hue = HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
        } else {
            // Second half: red back to green (0.5 to 1.0)
            float ratio = (progress - 0.5f) * 2.0f;  // 0.0 to 1.0
            hue = (uint16_t)(ratio * HUE_GREEN);
        }
        
        // Get color from hue (intensity is applied by LED task)
        uint32_t color = get_color_from_hue(hue);
        led_set_color(color);
        
        // Delay until next update
        vTaskDelay(pdMS_TO_TICKS(UPDATE_INTERVAL_MS));
    }
    
    // Ensure we end on green
    uint32_t final_color = get_color_from_hue(HUE_GREEN);
    led_set_color(final_color);
}

/**
 * @brief Build ENS16X temperature/humidity compensation bytes from floats.
 *
 * The ENS16X TEMP_IN/RH_IN registers (written by ens16x_write_ens210_data())
 * use the same encoding as the ENS210 raw output: temperature in 1/64 K and
 * relative humidity in 1/512 %, each a little-endian uint16. On Pro hardware
 * the ENS210 is absent, so we synthesize these bytes from the SCD41 readings.
 */
static void compose_ens16x_compensation(float temp_c, float humidity,
                                        uint8_t t[2], uint8_t h[2])
{
    uint16_t t_raw = (uint16_t)((temp_c + 273.15f) * 64.0f);
    uint16_t h_raw = (uint16_t)(humidity * 512.0f);
    t[0] = (uint8_t)(t_raw & 0xFF);
    t[1] = (uint8_t)(t_raw >> 8);
    h[0] = (uint8_t)(h_raw & 0xFF);
    h[1] = (uint8_t)(h_raw >> 8);
}

// Command processing task
void command_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Command task started");
    
    while (1) {
        // Process incoming commands
        serial_process_commands();
        
        // Small delay to prevent CPU spinning
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// Sensor reading task
void sensor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Sensor task started");

    esp_task_wdt_add(NULL);

    bool is_pro = aircube_model_is_pro();

    while (1) {
        esp_task_wdt_reset();

        // Temperature/RH source depends on the hardware model:
        //   Base -> ENS210, Pro -> SCD41 (ENS210 absent).
        // Both feed the ENS16X TEMP_IN/RH_IN registers for VOC compensation.
        float temp_c = 0.0f;
        float humidity = 0.0f;
        uint8_t ens210_status = 0;   // kept for serial JSON compatibility
        uint16_t co2_ppm = 0;        // true CO2 (Pro/SCD41 only)
        float lux = 0.0f;            // ambient light (Pro/VCNL4040 only)
        uint8_t comp_t[2];
        uint8_t comp_h[2];

        // ENS210 comparison readout (Pro hardware that still has an ENS210 fitted).
        bool ens210_compare = false;
        float ens210_temp_c = 0.0f;
        float ens210_humidity = 0.0f;

        if (is_pro) {
            // Pro: SCD41 provides temp/RH/CO2; VCNL4040 provides ambient light.
            scd41_poll();   // drives single-shot cadence; refreshes cached values
            temp_c   = scd41_get_temperature_c();
            humidity = scd41_get_humidity();
            co2_ppm  = scd41_get_co2();
            ens210_status = 0x01;   // synthetic "OK" so downstream logic is unchanged

            vcnl4040_read();
            lux = vcnl4040_get_lux();
            auto_dim_update_lux(lux);

            // If an ENS210 is also fitted, read it too so we can compare its
            // temperature/humidity against the SCD41 (debug/calibration aid).
            if (ens210_is_present()) {
                ens210_read_envir();
                ens210_temp_c = ens210_get_temperature(1) - 2; // same -2 C enclosure offset as Base
                ens210_humidity = ens210_get_humidity();
                ens210_compare = true;
            }

            // Only feed the ENS16X real compensation once the SCD41 has produced
            // a valid measurement. During the first ~5 s warm-up its values are
            // still 0, which would push bad temp/RH into the VOC compensation.
            if (scd41_has_data()) {
                compose_ens16x_compensation(temp_c, humidity, comp_t, comp_h);
                ens16x_write_ens210_data(comp_t, comp_h);

                // True CO2 in, for the LED's worst-of-VOC/CO2 arbitration below.
                // Gated on scd41_has_data(), same as the compensation write
                // above, so it only reflects a real reading rather than the
                // transient 0 ppm seen during the ~5s SCD41 warm-up.
                current_co2_level = co2_calculate((int)co2_ppm);
            }
        } else {
            // Base: ENS210 temperature and humidity.
            ens210_read_envir();
            temp_c = ens210_get_temperature(1); // 1 = Celsius
            humidity = ens210_get_humidity();
            ens210_status = ens210_get_status();

            // ENS210 reads ~2 C high in this enclosure; correct it.
            temp_c -= 2;

            // Write ENS210 data to ENS161 for environmental compensation
            ens210_get_envir(comp_t, comp_h);
            ens16x_write_ens210_data(comp_t, comp_h);
        }
        
        // Refresh ENS16X device status (needed to detect warm-up completion)
        ens16x_get_device_status();
        
        // Read ENS16X air quality data
        int etvoc = ens16x_read_etvoc();
        int eco2 = ens16x_read_eco2();
        int aqi_s = ens16x_read_aqi();         // Deprecated AQI-S; always 0
        int aqi = aqi_calculate(etvoc);        // Canonical AirCube VOC Level (TVOC-derived, 0-500)
        int aqi_uba = ens16x_read_aqi_uba();
        enum ENS_STATUS ens16x_status = ens16x_get_status();

        current_voc_level = aqi;
        
        // Helper function to convert ENS16X status to string
        const char* ens16x_status_str;
        switch(ens16x_status) {
            case ENS_OP_OK:
                ens16x_status_str = "OK";
                break;
            case ENS_WARM_UP:
                ens16x_status_str = "Warming Up";
                break;
            case ENS_NO_VALID_OUTPUT:
                ens16x_status_str = "No Valid Output";
                break;
            case ENS_RESERVED:
                ens16x_status_str = "Reserved";
                break;
            default:
                ens16x_status_str = "Unknown";
                break;
        }
        
        // Display all sensor data with status
        ESP_LOGI(TAG, "=== Sensor Data (%s) ===", aircube_model_name());
        ESP_LOGI(TAG, "%s - Temperature: %.2f°C / %.2f°F, Humidity: %.2f%%",
                 is_pro ? "SCD41 " : "ENS210", temp_c, temp_c * 1.8f + 32.0f, humidity);
        ESP_LOGI(TAG, "ENS16X - Status: %s, eTVOC: %d ppb, eCO2: %d ppm, VOC Level: %d, AQI-S: %d, AQI-UBA: %d",
                 ens16x_status_str, etvoc, eco2, aqi, aqi_s, aqi_uba);
        if (is_pro) {
            ESP_LOGI(TAG, "SCD41  - CO2: %u ppm", co2_ppm);
            ESP_LOGI(TAG, "VCNL4040 - Ambient: %.1f lux", lux);
            if (ens210_compare) {
                float dT_c = ens210_temp_c - temp_c;
                ESP_LOGI(TAG, "ENS210 - Temperature: %.2f°C / %.2f°F, Humidity: %.2f%% (compare: dT=%+.2f°C / %+.2f°F, dRH=%+.2f%%)",
                         ens210_temp_c, ens210_temp_c * 1.8f + 32.0f, ens210_humidity,
                         dT_c, dT_c * 1.8f, ens210_humidity - humidity);
            }
        }

        // On Pro the SCD41 provides true NDIR CO2; log/store/report that instead
        // of the ENS16X eCO2 estimate. eTVOC and VOC Level still come from ENS16X.
        int co2_for_history = is_pro ? (int)co2_ppm : eco2;

        // Send sensor data as JSON over serial
        serial_send_sensor_data(ens210_status, temp_c, humidity,
                               ens16x_status_str, etvoc, eco2, aqi, aqi_s, aqi_uba,
                               aircube_model_name(), (int)co2_ppm, lux);
        
        // Record sample into history accumulator and check for 10-min flush.
        // History stores the new canonical VOC Level (TVOC-derived); flushed entries
        // from before this change still hold the old AQI-S value in the same
        // columns - the on-flash byte layout did not change.
        history_record_sample(temp_c, humidity, aqi, co2_for_history, etvoc);
        history_check_flush();
        
        // Push sensor data to the active radio (modes are exclusive by design)
        if (radio_mode_is_zigbee_mode()) {
            // Zigbee: report every 10 seconds
            static TickType_t last_zb_update = 0;
            TickType_t now = xTaskGetTickCount();
            if ((now - last_zb_update) >= pdMS_TO_TICKS(10000)) {
                last_zb_update = now;
                zigbee_update_sensors(temp_c, humidity, eco2, etvoc, aqi, (float)co2_ppm, lux);
            }
        } else {
            // BLE: notify a connected client + refresh BTHome advertisement
            ble_gatt_update_live(temp_c, humidity, aqi, eco2, etvoc,
                                 co2_ppm, lux, aqi_uba);
        }

        // Wait for configurable period before next reading
        uint32_t period = get_sensor_readout_period_ms();
        vTaskDelay(period / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AirCube");

    ESP_LOGI(TAG, "LED color: continuous green->red from canonical VOC Level (TVOC-derived)");

    // Configure power management
    // ESP32-H2 valid max frequencies: 96, 64, or 48 MHz. Min = XTAL = 32 MHz.
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 48,            // Maximum CPU frequency (MHz)
        .min_freq_mhz = 32,            // Minimum CPU frequency (XTAL, MHz)
        .light_sleep_enable = false     // Automatic light sleep when idle
    };
    
    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure power management: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Power management configured with automatic light sleep enabled");
    }

    // Initialize NVS (Non-Volatile Storage) for saving settings
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // Initialize I2C driver (must be done before initializing sensors)
    if (i2c_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C driver");
        return;
    }

    // Create mutex for readout period
    readout_period_mutex = xSemaphoreCreateMutex();
    if (readout_period_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create readout period mutex");
        return;
    }
    
    // Initialize serial protocol
    serial_protocol_init();
    
    // Initialize LED control system
    led_init();
    
    // Set initial LED color to green (animation start color) before animation
    uint32_t start_color = get_color_from_hue(HUE_GREEN);
    led_set_color(start_color);

    
    // // Play startup animation (3 second sweep from green to red and back)
    // ESP_LOGI(TAG, "Playing startup animation");
    // startup_animation();
    
    // Initialize history module (sensor data logging to flash)
    esp_err_t hist_ret = history_init();
    if (hist_ret != ESP_OK) {
        ESP_LOGW(TAG, "History init failed: %s (continuing without history)", esp_err_to_name(hist_ret));
    }
    
    // Initialize button for brightness control
    button_init();
    
    // Initialize ENS210 temperature and humidity sensor (Base; may be absent on Pro)
    ens210_init();
    ESP_LOGI(TAG, "ENS210 %s", ens210_is_present() ? "present" : "not present");
    
    // Initialize ENS16X air quality sensor
    ens16x_init();
    ESP_LOGI(TAG, "ENS16X initialized");

    // Initialize Pro-model sensors. These probe for presence and are no-ops on
    // Base hardware (which lacks them). Phase 1: debug output only.
    scd41_init();
    ESP_LOGI(TAG, "SCD41 %s", scd41_is_present() ? "present" : "not present");
    vcnl4040_init();
    ESP_LOGI(TAG, "VCNL4040 %s", vcnl4040_is_present() ? "present" : "not present");

    // Decide Base vs Pro from sensor presence (drivers probed above). This
    // gates temp/RH source selection and Pro-only Zigbee clusters.
    aircube_model_detect();

    // Apply configured brightness through auto-dim (Pro) or pass-through (Base).
    auto_dim_init();
    
    // BLE-first radio mode: exactly one radio stack runs per boot (BLE+Zigbee
    // coexistence hung the PHY on the H2). Default is BLE with a connectable
    // GATT service; Zigbee only when commissioned or a pairing was requested.
    // Transitions happen via NVS flags + reboot (see radio_mode.h).
    radio_mode_init();
    if (radio_mode_is_zigbee_mode()) {
        // Zigbee End Device (steers immediately if a pairing was requested)
        zigbee_init();
    } else {
        // Connectable GATT service + BTHome advertising
        ble_gatt_init();
        // Mode cue: two quick blue blinks when entering BLE mode
        for (int i = 0; i < 2; i++) {
            led_set_color(LED_COLOR_BLUE);
            vTaskDelay(pdMS_TO_TICKS(120));
            led_set_color(LED_COLOR_OFF);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
    
    // Create command processing task
    xTaskCreate(command_task, "command_task", COMMAND_TASK_STACK_SIZE, NULL, 
                COMMAND_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Command task created");
    
    // Create sensor reading task
    xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK_SIZE, NULL, 
                SENSOR_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Sensor task created");

    // Main loop for LED color based on VOC Level (with pairing override)
    bool was_zb_connected = zigbee_is_connected();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));  // Update LED every 20ms for smooth transitions
        
        // ── Mode cue: three quick green blinks when Zigbee joins ──
        bool zb_connected = zigbee_is_connected();
        if (zb_connected && !was_zb_connected) {
            for (int i = 0; i < 3; i++) {
                led_set_color(LED_COLOR_GREEN);
                vTaskDelay(pdMS_TO_TICKS(120));
                led_set_color(LED_COLOR_OFF);
                vTaskDelay(pdMS_TO_TICKS(120));
            }
        }
        was_zb_connected = zb_connected;
        
        // ── Pairing mode: flash blue at 2 Hz ──
        if (zigbee_is_pairing()) {
            uint32_t tick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            bool on = ((tick_ms / 250) % 2) == 0;
            led_set_color(on ? LED_COLOR_BLUE : LED_COLOR_OFF);
            continue;   // Skip normal VOC Level color while pairing
        }
        
        // ── Normal mode: continuous green->red from whichever is worse ──
        // VOC and CO2 are scored independently on the same 0-500 scale; the
        // worse of the two (never a blend of both) drives the color. CO2 is
        // only considered on Pro hardware (current_co2_level stays 0 on Base).
        int display_level = current_voc_level;
        bool co2_is_worse = false;
        if (aircube_model_is_pro() && current_co2_level > display_level) {
            display_level = current_co2_level;
            co2_is_worse = true;
        }

        static bool was_co2_worse = false;
        if (co2_is_worse != was_co2_worse) {
            ESP_LOGI(TAG, "LED now driven by %s Level (VOC %d, CO2 %d)",
                     co2_is_worse ? "CO2" : "VOC", current_voc_level, current_co2_level);
            was_co2_worse = co2_is_worse;
        }

        if (display_level >= AQI_MAX) {
            target_hue = 0;  // Red for high severity
        } else {
            target_hue = aqi_to_hue(display_level);
        }
        
        // Smoothly transition current_hue towards target_hue
        float hue_diff = (float)target_hue - current_hue;
        current_hue += hue_diff * TRANSITION_SPEED;
        
        // Convert current hue to color (cast to uint16_t for color conversion)
        uint32_t color = get_color_from_hue((uint16_t)current_hue);
        
        // Update LED with the smoothly transitioning color
        led_set_color(color);
    }
}

