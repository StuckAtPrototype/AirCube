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
#include "auto_dim.h"
#include "zigbee.h"
#include "ble_gatt.h"
#include "radio_mode.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdbool.h>

static const char *TAG = "button";

// Button GPIO configuration
#define BUTTON_GPIO 11

// Debounce timing (in milliseconds)
#define DEBOUNCE_MS 50

// Long press threshold for Zigbee pairing (in milliseconds)
#define LONG_PRESS_MS 3000

/* Words; 2048 was too small for zigbee_start_pairing() + esp_zb_lock stack use. */
#define BUTTON_TASK_STACK_WORDS 4096

// NVS namespace and keys for brightness storage
#define NVS_NAMESPACE "aircube"
#define NVS_KEY_BRIGHTNESS "led_brightness"   // legacy: index into brightness_levels
#define NVS_KEY_BRIGHT_PCT "led_bright_pct"   // percent 0-100 (any value, e.g. from BLE)

// Brightness levels the button cycles through
static const float brightness_levels[] = {0.0f, 0.1f, 0.3f, 0.6f, 1.0f};
static const int num_brightness_levels = sizeof(brightness_levels) / sizeof(brightness_levels[0]);

#define DEFAULT_BRIGHTNESS_PCT 60

// Current brightness percent (0-100); remote writes can set any value,
// the button snaps to the next level above the current percent.
static int current_brightness_pct = DEFAULT_BRIGHTNESS_PCT;

// GPIO interrupt queue
static QueueHandle_t gpio_evt_queue = NULL;

/**
 * @brief Save brightness percent to NVS
 * 
 * @param percent Brightness percent (0-100) to save
 * @return true on success, false on failure
 */
static bool save_brightness_to_nvs(int percent)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_i32(nvs_handle, NVS_KEY_BRIGHT_PCT, percent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving brightness to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Brightness saved to NVS: %d%%", percent);
    return true;
}

/**
 * @brief Load brightness percent from NVS
 *
 * Reads the percent key; falls back to migrating the legacy level-index
 * key written by older firmware.
 * 
 * @param percent Pointer to store the loaded brightness percent
 * @return true if value was loaded, false if not found or error
 */
static bool load_brightness_from_nvs(int *percent)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error opening NVS handle: %s (using default)", esp_err_to_name(err));
        return false;
    }
    
    int32_t saved_pct = -1;
    err = nvs_get_i32(nvs_handle, NVS_KEY_BRIGHT_PCT, &saved_pct);
    if (err == ESP_OK && saved_pct >= 0 && saved_pct <= 100) {
        *percent = (int)saved_pct;
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Brightness loaded from NVS: %d%%", *percent);
        return true;
    }

    // Legacy firmware stored an index into brightness_levels
    int32_t saved_index = -1;
    err = nvs_get_i32(nvs_handle, NVS_KEY_BRIGHTNESS, &saved_index);
    nvs_close(nvs_handle);
    if (err == ESP_OK && saved_index >= 0 && saved_index < num_brightness_levels) {
        *percent = (int)(brightness_levels[saved_index] * 100.0f + 0.5f);
        ESP_LOGI(TAG, "Brightness migrated from legacy NVS index %ld: %d%%",
                 saved_index, *percent);
        return true;
    }

    ESP_LOGI(TAG, "No saved brightness found in NVS, using default");
    return false;
}

/**
 * @brief Apply a brightness percent: LED, NVS, and radio reports.
 */
static void apply_brightness_percent(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    current_brightness_pct = percent;
    save_brightness_to_nvs(percent);
    auto_dim_recompute();
    zigbee_report_brightness();     /* no-op unless joined to Zigbee */
    ble_gatt_report_brightness();   /* no-op unless a BLE central subscribed */
}

void button_set_brightness_percent(int percent)
{
    apply_brightness_percent(percent);
    ESP_LOGI(TAG, "Brightness set to %d%% (remote)", current_brightness_pct);
}

int button_get_brightness_percent(void)
{
    return current_brightness_pct;
}

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
 * @brief Button task to handle debouncing, brightness toggling, and Zigbee pairing
 * 
 * Short press (< 3 s): cycles through brightness levels 0.0 -> 0.1 -> 0.3 -> 0.6 -> 1.0 -> 0.0
 * Long  press (>= 3 s): triggers Zigbee network steering (pairing mode)
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
                    last_press_time = current_time;
                    
                    // Measure how long the button is held
                    TickType_t press_start = xTaskGetTickCount();
                    bool long_press = false;
                    
                    while (gpio_get_level(io_num) == 1) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        if ((xTaskGetTickCount() - press_start) >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
                            long_press = true;
                            break;
                        }
                    }
                    
                    if (long_press) {
                        // ── Long press: start Zigbee pairing ──
                        // In BLE mode this reboots into Zigbee mode first
                        // (BLE-first radio mode switching, see radio_mode.h).
                        ESP_LOGI(TAG, "Long press detected – starting Zigbee pairing");
                        radio_mode_start_pairing();
                        
                        // Wait for button release before accepting new events
                        while (gpio_get_level(io_num) == 1) {
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        
                        // Drain any queued events accumulated during the hold
                        while (xQueueReceive(gpio_evt_queue, &io_num, 0) == pdTRUE) {}
                    } else {
                        // ── Short press: cycle brightness ──
                        // Snap to the next level above the current percent
                        // (which may be an arbitrary value set over BLE),
                        // wrapping back to off after full brightness.
                        int next_pct = 0;
                        for (int i = 0; i < num_brightness_levels; i++) {
                            int level_pct = (int)(brightness_levels[i] * 100.0f + 0.5f);
                            if (level_pct > current_brightness_pct) {
                                next_pct = level_pct;
                                break;
                            }
                        }

                        apply_brightness_percent(next_pct);
                        ESP_LOGI(TAG, "Short press – Brightness set to %d%%", current_brightness_pct);
                    }
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
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", BUTTON_TASK_STACK_WORDS, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return;
    }
    
    // Load saved brightness from NVS, or use default. Effective output is
    // applied later by auto_dim_init() after hardware model detection.
    int saved_pct = DEFAULT_BRIGHTNESS_PCT;
    if (load_brightness_from_nvs(&saved_pct)) {
        current_brightness_pct = saved_pct;
    }

    ESP_LOGI(TAG, "Button initialized successfully (configured %d%%)", current_brightness_pct);
}

