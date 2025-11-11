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
            
            // Read ENS16X air quality data
            int etvoc = ens16x_read_etvoc();
            int eco2 = ens16x_read_eco2();
            int aqi = ens16x_read_aqi();
            
            // Display all sensor data
            ESP_LOGI(TAG, "=== Sensor Data ===");
            ESP_LOGI(TAG, "ENS210 - Temperature: %.2f°C, Humidity: %.2f%%", temp_c, humidity);
            ESP_LOGI(TAG, "ENS16X - eTVOC: %d ppb, eCO2: %d ppm, AQI: %d", etvoc, eco2, aqi);
        }
    }

}

