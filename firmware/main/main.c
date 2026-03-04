#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "led_color_lib.h"

#include "led.h"
#include "ens210.h"
#include "ens16x_driver.h"
#include "i2c_driver.h"
#include "serial_protocol.h"
#include "button.h"
#include "history.h"
#include "zigbee.h"

static const char *TAG = "main";

#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY 5
#define COMMAND_TASK_STACK_SIZE 4096
#define COMMAND_TASK_PRIORITY 4

// Configurable sensor readout period (default 1000ms)
static uint32_t sensor_readout_period_ms = 1000;
static SemaphoreHandle_t readout_period_mutex = NULL;

// AQI color mapping constants
#define AQI_MIN 0
#define AQI_MAX 200
#define AQI_GREEN_THRESHOLD 10  // Values 0-10 are pure green

// Global variables to store sensor data for LED color mapping
static int current_aqi = 0;
static enum ENS_STATUS current_ens16x_status = ENS_RESERVED;

// Static variables for smooth LED color transitions
static float current_hue = 21845.0f;  // Current hue value (21845 = green, 0 = red) - using float for smooth transitions
static uint16_t target_hue = 21845;   // Target hue value we want to transition to
#define TRANSITION_SPEED 0.02f  // Transition speed per update (0.0 to 1.0, higher = faster)
// With 20ms update interval and 0.02 speed, full transition takes ~1 second (50 steps)
#define HUE_GREEN 21845  // 2/6 of 65536 (120 degrees - green)

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
 * @brief Map AQI value to hue
 * 
 * Maps AQI with the following behavior:
 * - AQI 0-10: pure green (no color change)
 * - AQI 10-200: smooth gradient from green to red
 * 
 * @param aqi Air Quality Index value
 * @return 16-bit hue value (21845 = green, 0 = red)
 */
static uint16_t aqi_to_hue(int aqi)
{
    // Clamp AQI to valid range
    if (aqi < AQI_MIN) aqi = AQI_MIN;
    if (aqi > AQI_MAX) aqi = AQI_MAX;
    
    // Values 0-10 are pure green
    if (aqi <= AQI_GREEN_THRESHOLD) {
        return HUE_GREEN;
    }
    
    // For values 10-200, map smoothly from green to red
    // Map AQI from [10, 200] to ratio [0.0, 1.0] for smooth gradient
    // When AQI = 10: ratio = 0.0 (green)
    // When AQI = 200: ratio = 1.0 (red)
    float ratio = (float)(aqi - AQI_GREEN_THRESHOLD) / (float)(AQI_MAX - AQI_GREEN_THRESHOLD);
    
    // Linear interpolation from green to red
    // ratio = 0.0 -> hue = HUE_GREEN (green)
    // ratio = 1.0 -> hue = 0 (red)
    uint16_t hue = HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
    
    return hue;
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
    
    while (1) {
        // Read ENS210 temperature and humidity
        ens210_read_envir();
        float temp_c = ens210_get_temperature(1); // 1 = Celsius
        float humidity = ens210_get_humidity();
        uint8_t ens210_status = ens210_get_status();

        // we know that the temperature has a 2 degree offset from the real temperature
        // subtract 2 degrees from the temperature to get the real temperature
        temp_c -= 2;
        
        // Write ENS210 data to ENS161 for environmental compensation
        uint8_t ens210_t[2];
        uint8_t ens210_h[2];
        ens210_get_envir(ens210_t, ens210_h);
        ens16x_write_ens210_data(ens210_t, ens210_h);
        
        // Refresh ENS16X device status (needed to detect warm-up completion)
        ens16x_get_device_status();
        
        // Read ENS16X air quality data
        int etvoc = ens16x_read_etvoc();
        int eco2 = ens16x_read_eco2();
        int aqi = ens16x_read_aqi();
        int aqi_uba = ens16x_read_aqi_uba();
        enum ENS_STATUS ens16x_status = ens16x_get_status();
        
        // Update global variables for LED color mapping
        current_aqi = aqi;
        current_ens16x_status = ens16x_status;
        
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
        ESP_LOGI(TAG, "=== Sensor Data ===");
        ESP_LOGI(TAG, "ENS210 - Status: 0x%02X, Temperature: %.2f°C, Humidity: %.2f%%", 
                 ens210_status, temp_c, humidity);
        ESP_LOGI(TAG, "ENS16X - Status: %s, eTVOC: %d ppb, eCO2: %d ppm, AQI-S: %d, AQI-UBA: %d", 
                 ens16x_status_str, etvoc, eco2, aqi, aqi_uba);
        
        // Send sensor data as JSON over serial
        serial_send_sensor_data(ens210_status, temp_c, humidity,
                               ens16x_status_str, etvoc, eco2, aqi, aqi_uba);
        
        // Record sample into history accumulator and check for 10-min flush
        history_record_sample(temp_c, humidity, aqi, eco2, etvoc);
        history_check_flush();
        
        // Push sensor data to Zigbee attributes (reports automatically)
        zigbee_update_sensors(temp_c, humidity, eco2, etvoc, aqi);
        
        // Wait for configurable period before next reading
        uint32_t period = get_sensor_readout_period_ms();
        vTaskDelay(period / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AirCube");

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
    
    // Initialize ENS210 temperature and humidity sensor
    ens210_init();
    ESP_LOGI(TAG, "ENS210 initialized");
    
    // Initialize ENS16X air quality sensor
    ens16x_init();
    ESP_LOGI(TAG, "ENS16X initialized");
    
    // Initialize Zigbee stack (End Device, idles until long-press on first boot)
    zigbee_init();
    
    // Create command processing task
    xTaskCreate(command_task, "command_task", COMMAND_TASK_STACK_SIZE, NULL, 
                COMMAND_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Command task created");
    
    // Create sensor reading task
    xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK_SIZE, NULL, 
                SENSOR_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Sensor task created");

    // Main loop for LED color based on AQI (with pairing override)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));  // Update LED every 20ms for smooth transitions
        
        // ── Pairing mode: flash blue at 2 Hz ──
        if (zigbee_is_pairing()) {
            uint32_t tick_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            bool on = ((tick_ms / 250) % 2) == 0;
            led_set_color(on ? LED_COLOR_BLUE : LED_COLOR_OFF);
            continue;   // Skip normal AQI color while pairing
        }
        
        // ── Normal mode: AQI-based color ──
        // Determine target hue based on AQI (green at 0, red at 200+)
        if (current_aqi >= AQI_MAX) {
            target_hue = 0;  // Red for high AQI
        } else {
            target_hue = aqi_to_hue(current_aqi);
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

