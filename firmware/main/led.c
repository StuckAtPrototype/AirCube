/**
 * @file led.c
 * @brief LED control system implementation
 * 
 * This file implements the LED control system for the AirCube device.
 * It manages WS2812 addressable LEDs with thread-safe color and intensity control.
 * All 3 LEDs are kept at the same color and intensity.
 * 
 * @author StuckAtPrototype, LLC
 * @version 4.0
 */

#include "led.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "led_color_lib.h"

// Number of LEDs to control (using first 3 LEDs)
#define NUM_CONTROLLED_LEDS 3

// Mutex to protect LED color and intensity updates
static SemaphoreHandle_t led_mutex = NULL;

// Global LED color and intensity variables (GRB format for WS2812 LEDs)
static uint32_t led_color = LED_COLOR_OFF;      // Current LED color
static float led_intensity = 0.6f;              // Current LED intensity (0.0 to 1.0)

// LED state structure for WS2812 driver
static struct led_state led_new_state = {0};

/**
 * @brief LED control task
 * 
 * This task continuously updates the WS2812 LEDs with the current color
 * and intensity settings. It reads the color and intensity in a thread-safe
 * manner and applies them to all 3 LEDs.
 * 
 * @param pvParameters Task parameters (unused)
 */
static void led_task(void *pvParameters)
{
    while (1) {
        // Take mutex to safely read LED color and intensity
        if (led_mutex != NULL && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Read current LED color and intensity safely
            uint32_t current_color = led_color;
            float current_intensity = led_intensity;
            xSemaphoreGive(led_mutex);
            
            // Apply intensity to color
            uint32_t final_color = apply_color_intensity(current_color, current_intensity);
            
            // Set all 3 LEDs to the same color
            for (int i = 0; i < NUM_CONTROLLED_LEDS; i++) {
                led_new_state.leds[i] = final_color;
            }
            
            // Set remaining LEDs to off
            for (int i = NUM_CONTROLLED_LEDS; i < NUM_LEDS; i++) {
                led_new_state.leds[i] = LED_COLOR_OFF;
            }
            
            // Update WS2812 LEDs
            ws2812_write_leds(led_new_state);
        } else {
            // Fallback mode if mutex is not available
            if (led_mutex == NULL) {
                ESP_LOGW("led", "LED mutex not available, using direct access");
                // Apply intensity to color
                uint32_t final_color = apply_color_intensity(led_color, led_intensity);
                
                // Set all 3 LEDs to the same color
                for (int i = 0; i < NUM_CONTROLLED_LEDS; i++) {
                    led_new_state.leds[i] = final_color;
                }
                
                // Set remaining LEDs to off
                for (int i = NUM_CONTROLLED_LEDS; i < NUM_LEDS; i++) {
                    led_new_state.leds[i] = LED_COLOR_OFF;
                }
                
                // Update WS2812 LEDs
                ws2812_write_leds(led_new_state);
            } else {
                ESP_LOGW("led", "Failed to take LED mutex - skipping update");
            }
        }

        // Task delay for 20ms (50Hz update rate) - sufficient for smooth animations
        // The main loop updates color more frequently, this task just displays the latest value
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * @brief Initialize the LED control system
 * 
 * This function initializes the WS2812 LED driver, creates the LED mutex
 * for thread-safe operations, and starts the LED control task.
 */
void led_init(void) {
    // Initialize WS2812 LED driver
    ws2812_control_init();

    // Create mutex for thread-safe LED color and intensity updates
    led_mutex = xSemaphoreCreateMutex();
    if (led_mutex == NULL) {
        ESP_LOGE("led", "Failed to create LED mutex - system will be unstable");
        // Continue execution but log the error - system will use fallback mode
    }

    // Create LED control task
    BaseType_t ret = xTaskCreate(led_task, "led_task", 4096, NULL, 10, NULL);
    if (ret != pdPASS) {
        ESP_LOGE("led", "Failed to create LED task");
    }
}

/**
 * @brief Set LED color
 * 
 * This function sets the color of all LEDs in a thread-safe manner.
 * The color should be in GRB format. Use led_color_lib functions
 * to generate colors (e.g., get_color_green_to_red()).
 * 
 * @param color Color value in GRB format (0x00RRGGBB)
 */
void led_set_color(uint32_t color) {
    if (led_mutex != NULL && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        led_color = color;
        xSemaphoreGive(led_mutex);
    } else if (led_mutex == NULL) {
        // Fallback: direct assignment if mutex not available
        led_color = color;
    }
}

/**
 * @brief Set LED intensity
 * 
 * This function sets the intensity (brightness) of all LEDs in a thread-safe manner.
 * The intensity value should be between 0.0 (off) and 1.0 (full brightness).
 * 
 * @param intensity Intensity value (0.0 to 1.0)
 */
void led_set_intensity(float intensity) {
    // Clamp intensity to valid range
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    
    if (led_mutex != NULL && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        led_intensity = intensity;
        xSemaphoreGive(led_mutex);
    } else if (led_mutex == NULL) {
        // Fallback: direct assignment if mutex not available
        led_intensity = intensity;
    }
}

/**
 * @brief Get current LED color
 * 
 * This function returns the current LED color in a thread-safe manner.
 * 
 * @return Current color value in GRB format
 */
uint32_t led_get_color(void) {
    uint32_t color = LED_COLOR_OFF;
    if (led_mutex != NULL && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        color = led_color;
        xSemaphoreGive(led_mutex);
    } else if (led_mutex == NULL) {
        color = led_color;
    }
    return color;
}

/**
 * @brief Get current LED intensity
 * 
 * This function returns the current LED intensity in a thread-safe manner.
 * 
 * @return Current intensity value (0.0 to 1.0)
 */
float led_get_intensity(void) {
    float intensity = 0.0f;
    if (led_mutex != NULL && xSemaphoreTake(led_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        intensity = led_intensity;
        xSemaphoreGive(led_mutex);
    } else if (led_mutex == NULL) {
        intensity = led_intensity;
    }
    return intensity;
}
