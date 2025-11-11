#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_color_lib.h"

#include "led.h"
#include "ens210.h"
#include "ens16x_driver.h"
#include "i2c_driver.h"

static const char *TAG = "main";

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

    uint16_t hue = 0;
    // never ending task

    while (1) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        hue += 1;
        // hue will wrap around naturally when it reaches 65535
        led_set_color(get_color_from_hue(hue));
        
        // Read and print sensor data every 1000ms (100 iterations * 10ms)
        static int counter = 0;
        counter++;
        if (counter >= 100) {
            counter = 0;
            
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
        }
    }

}

