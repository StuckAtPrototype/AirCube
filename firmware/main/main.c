#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_color_lib.h"

#include "led.h"

static const char *TAG = "main";

void app_main(void)
{

    ESP_LOGI(TAG, "AirCube");

    // Initialize LED control system
    led_init();
    
    // // Set LED color to red with full intensity
    // led_set_color(LED_COLOR_RED);

    uint16_t hue = 0;
    // never ending task

    while (1) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        hue += 1;
        if (hue >= 65536) {
            hue = 0;
        }
        led_set_color(get_color_from_hue(hue));
    }

}

