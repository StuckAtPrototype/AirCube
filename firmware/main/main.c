#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_color_lib.h"

#include "led.h"
#include "ens210.h"
#include "ens16x_driver.h"
#include "i2c_driver.h"

static const char *TAG = "main";

#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY 5

// AQI color mapping constants
#define AQI_MIN 0
#define AQI_MAX 300

// Global variable to store current AQI for LED color mapping
static int current_aqi = 0;

/**
 * @brief Map AQI value to color
 * 
 * Maps AQI from 0 (green) to 300+ (red) using hue values
 * AQI 0 = green (hue 21845, which is 2/6 of spectrum = 120 degrees)
 * AQI 300+ = red (hue 0, which is 0 degrees)
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
    const uint16_t HUE_RED = 0;       // 0 degrees - red
    
    // Linear interpolation from green (AQI 0) to red (AQI 300)
    // When AQI = 0: hue = HUE_GREEN (green)
    // When AQI = 300: hue = HUE_RED (red)
    // We go backwards from green to red
    float ratio = (float)aqi / (float)AQI_MAX;
    uint16_t hue = HUE_GREEN - (uint16_t)(ratio * HUE_GREEN);
    
    return get_color_from_hue(hue);
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
        
        // Update global AQI for LED color mapping
        current_aqi = aqi;
        
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
        
        // Wait 1 second before next reading
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AirCube");

    // Initialize I2C driver (must be done before initializing sensors)
    if (i2c_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C driver");
        return;
    }

    // Initialize LED control system
    led_init();
    
    // Initialize ENS210 temperature and humidity sensor
    ens210_init();
    ESP_LOGI(TAG, "ENS210 initialized");
    
    // Initialize ENS16X air quality sensor
    ens16x_init();
    ESP_LOGI(TAG, "ENS16X initialized");
    
    // Create sensor reading task
    xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK_SIZE, NULL, 
                SENSOR_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Sensor task created");

    // Main loop for LED color based on AQI
    while (1) {
        vTaskDelay(100 / portTICK_PERIOD_MS);  // Update LED every 100ms
        
        // Map AQI to color (green at 0, red at 300+)
        uint32_t color = aqi_to_color(current_aqi);
        led_set_color(color);
    }
}

