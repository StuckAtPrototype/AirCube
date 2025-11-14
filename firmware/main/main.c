#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "led_color_lib.h"
#include <math.h>

#include "led.h"
#include "ens210.h"
#include "ens16x_driver.h"
#include "i2c_driver.h"
#include "serial_protocol.h"
#include "button.h"

static const char *TAG = "main";

#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY 5
#define COMMAND_TASK_STACK_SIZE 2048
#define COMMAND_TASK_PRIORITY 4

// Configurable sensor readout period (default 1000ms)
static uint32_t sensor_readout_period_ms = 1000;
static SemaphoreHandle_t readout_period_mutex = NULL;

// AQI color mapping constants
#define AQI_MIN 0
#define AQI_MAX 200

// Global variables to store sensor data for LED color mapping
static int current_aqi = 0;
static enum ENS_STATUS current_ens16x_status = ENS_RESERVED;

// Static variables for pulsing effect
static uint32_t pulse_time_ms = 0;  // Current pulse time in milliseconds (accumulates)
#define PULSE_MS 50  // Pulse period in milliseconds

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
 * @brief Map AQI value to color
 * 
 * Maps AQI from 0 (green) to 200+ (red) using hue values
 * AQI 0 = green (hue 21845, which is 2/6 of spectrum = 120 degrees)
 * AQI 200+ = red (hue 0, which is 0 degrees)
 * 
 * @param aqi Air Quality Index value
 * @return 24-bit GRB color value
 */
static uint32_t aqi_to_color(int aqi)
{
    // Clamp AQI to valid range
    if (aqi < AQI_MIN) aqi = AQI_MIN;
    if (aqi > AQI_MAX) aqi = AQI_MAX;
    
    // Hue values: green is at 2/6 of the spectrum (120 degrees), red is at 0 (0 degrees)
    // For 16-bit hue: green = (2/6) * 65536 = 21845, red = 0
    const uint16_t HUE_GREEN = 21845;  // 2/6 of 65536 (120 degrees - green)
    
    // Linear interpolation from green (AQI 0) to red (AQI 200)
    // When AQI = 0: hue = HUE_GREEN (green)
    // When AQI = 200: hue = 0 (red)
    // We go backwards from green to red
    float ratio = (float)aqi / (float)AQI_MAX;
    uint16_t hue = HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
    
    return get_color_from_hue(hue);
}

/**
 * @brief Get pulsing color effect with intensity support
 * 
 * This function creates a pulsing effect by modulating the brightness of a given
 * color using a sine wave. The pulse goes from 0 to full brightness (255) at peak.
 * The LED task will apply the intensity setting, so the pulse will respect the
 * configured brightness level (pulse peak = intensity * 255).
 * 
 * @param red Red component of the base color (0-255)
 * @param green Green component of the base color (0-255)
 * @param blue Blue component of the base color (0-255)
 * @return 24-bit GRB color value with pulsing brightness (intensity applied by LED task)
 */
static uint32_t get_pulsing_color_with_intensity(uint8_t red, uint8_t green, uint8_t blue) {
    // Increment the time (function is called every 100ms)
    pulse_time_ms += 100;
    
    // Calculate the phase of the pulse (0 to 2π) using modulo to wrap around
    float phase = ((pulse_time_ms % PULSE_MS) / (float)PULSE_MS) * 2 * M_PI;

    // Use a sine wave to create a smooth pulse (range: 0 to 1.0 for full brightness)
    float pulse_brightness = (sinf(phase) + 1.0f) / 2.0f;  // Range: 0.0 to 1.0

    // Apply the pulse brightness to the specified color (0 to 255)
    // The LED task will apply the intensity setting, so we pulse to full brightness here
    float r = pulse_brightness * red;
    float g = pulse_brightness * green;
    float b = pulse_brightness * blue;

    // Convert to GRB format for WS2812 LEDs
    return ((uint32_t)(g + 0.5f) << 16) | ((uint32_t)(r + 0.5f) << 8) | (uint32_t)(b + 0.5f);
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
        
        // Write ENS210 data to ENS161 for environmental compensation
        uint8_t ens210_t[2];
        uint8_t ens210_h[2];
        ens210_get_envir(ens210_t, ens210_h);
        ens16x_write_ens210_data(ens210_t, ens210_h);
        
        // Read ENS16X air quality data
        int etvoc = ens16x_read_etvoc();
        int eco2 = ens16x_read_eco2();
        int aqi = ens16x_read_aqi();
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
        ESP_LOGI(TAG, "ENS16X - Status: %s, eTVOC: %d ppb, eCO2: %d ppm, AQI: %d", 
                 ens16x_status_str, etvoc, eco2, aqi);
        
        // Send sensor data as JSON over serial
        serial_send_sensor_data(ens210_status, temp_c, humidity,
                               ens16x_status_str, etvoc, eco2, aqi);
        
        // Wait for configurable period before next reading
        uint32_t period = get_sensor_readout_period_ms();
        vTaskDelay(period / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AirCube");

    // Configure power management with automatic light sleep
    // Note: ESP32-H2 uses the same structure as ESP32-C2 (both RISC-V based)
    esp_pm_config_esp32c2_t pm_config = {
        .max_freq_mhz = 10,           // Maximum CPU frequency (MHz)
        .min_freq_mhz = 10,            // Minimum CPU frequency (MHz)
        .light_sleep_enable = false    // Enable automatic light sleep when idle
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
    
    // Initialize button for brightness control
    button_init();
    
    // Initialize ENS210 temperature and humidity sensor
    ens210_init();
    ESP_LOGI(TAG, "ENS210 initialized");
    
    // Initialize ENS16X air quality sensor
    ens16x_init();
    ESP_LOGI(TAG, "ENS16X initialized");
    
    // Create command processing task
    xTaskCreate(command_task, "command_task", COMMAND_TASK_STACK_SIZE, NULL, 
                COMMAND_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Command task created");
    
    // Create sensor reading task
    xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK_SIZE, NULL, 
                SENSOR_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Sensor task created");

    // Main loop for LED color based on sensor status and AQI
    while (1) {
        vTaskDelay(100 / portTICK_PERIOD_MS);  // Update LED every 100ms
        
        uint32_t color;
        
        // If sensor is warming up, pulse blue
        if (current_ens16x_status == ENS_WARM_UP) {
            // Blue color: R=0, G=0, B=255
            color = get_pulsing_color_with_intensity(0, 0, 255);
        } else if (current_aqi >= AQI_MAX) {
            // If AQI is 200+, pulsate red to indicate dangerous air quality
            // Red color: R=255, G=0, B=0
            color = get_pulsing_color_with_intensity(255, 0, 0);
        } else {
            // Otherwise, use AQI-based color (green at 0, red at 200)
            color = aqi_to_color(current_aqi);
        }
        
        led_set_color(color);
    }
}

