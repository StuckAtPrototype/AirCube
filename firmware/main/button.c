/**
 * @file button.c
 * @brief Button control for brightness toggling
 * 
 * This file implements button functionality to toggle LED brightness levels.
 * The button is on GPIO 11, normally pulled low and goes high when pressed.
 * 
 * @author StuckAtPrototype, LLC
 * @version 1.0
 */

#include "button.h"
#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "button";

// Button GPIO configuration
#define BUTTON_GPIO 11

// Debounce timing (in milliseconds)
#define DEBOUNCE_MS 50

// Brightness levels array
static const float brightness_levels[] = {0.0f, 0.3f, 0.6f, 1.0f};
static const int num_brightness_levels = sizeof(brightness_levels) / sizeof(brightness_levels[0]);

// Current brightness index (starts at 0.6, which is index 2)
static int current_brightness_index = 2;  // 0.6 is the default

// GPIO interrupt queue
static QueueHandle_t gpio_evt_queue = NULL;

/**
 * @brief GPIO interrupt handler
 * 
 * This ISR is called when a GPIO interrupt occurs.
 * It sends the GPIO number to the queue for processing.
 */
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

/**
 * @brief Button task to handle debouncing and brightness toggling
 * 
 * This task processes button presses with debouncing and cycles through
 * brightness levels: 0.0 -> 0.3 -> 0.6 -> 1.0 -> 0.0
 */
static void button_task(void *pvParameters)
{
    uint32_t io_num;
    TickType_t last_press_time = 0;
    
    ESP_LOGI(TAG, "Button task started");
    
    while (1) {
        // Wait for GPIO event from ISR
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            TickType_t current_time = xTaskGetTickCount();
            
            // Debounce: only process if enough time has passed since last press
            if (current_time - last_press_time > pdMS_TO_TICKS(DEBOUNCE_MS)) {
                // Verify button is still pressed (debounce check)
                int level = gpio_get_level(io_num);
                if (level == 1) {
                    // Button is pressed - update timestamp
                    last_press_time = current_time;
                    
                    // Cycle to next brightness level
                    current_brightness_index = (current_brightness_index + 1) % num_brightness_levels;
                    float new_brightness = brightness_levels[current_brightness_index];
                    
                    // Update LED brightness
                    led_set_intensity(new_brightness);
                    
                    ESP_LOGI(TAG, "Button pressed - Brightness set to %.1f", new_brightness);
                }
            }
        }
    }
}

/**
 * @brief Initialize button functionality
 * 
 * This function configures GPIO 11 as an input with pull-down resistor
 * and sets up an interrupt on rising edge (button press).
 */
void button_init(void)
{
    ESP_LOGI(TAG, "Initializing button on GPIO %d", BUTTON_GPIO);
    
    // Create queue for GPIO events
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    if (gpio_evt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO event queue");
        return;
    }
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,      // Interrupt on rising edge (LOW -> HIGH)
        .mode = GPIO_MODE_INPUT,              // Input mode
        .pin_bit_mask = (1ULL << BUTTON_GPIO), // GPIO 11
        .pull_down_en = GPIO_PULLDOWN_ENABLE, // Enable pull-down (button normally LOW)
        .pull_up_en = GPIO_PULLUP_DISABLE,    // Disable pull-up
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        return;
    }
    
    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means ISR service already installed (OK)
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return;
    }
    
    // Hook ISR handler for specific GPIO
    ret = gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, (void*) BUTTON_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        return;
    }
    
    // Create button task
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return;
    }
    
    // Set initial brightness to default (0.6)
    led_set_intensity(brightness_levels[current_brightness_index]);
    
    ESP_LOGI(TAG, "Button initialized successfully");
}

