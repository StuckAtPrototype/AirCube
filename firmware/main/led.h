/**
 * @file led.h
 * @brief LED control system header
 * 
 * This header file defines the interface for the LED control system in the AirCube device.
 * It provides functions for controlling WS2812 addressable LEDs with thread-safe
 * color and intensity control. All 3 LEDs are kept at the same color and intensity.
 * 
 * @author StuckAtPrototype, LLC
 * @version 4.0
 */

#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ws2812_control.h"

// Predefined LED colors in GRB format (not RGB)
#define LED_COLOR_OFF    0x000000  // Black (LEDs off)
#define LED_COLOR_RED    0x00FF00  // Red
#define LED_COLOR_GREEN  0xFF0000  // Green
#define LED_COLOR_BLUE   0x0000FF  // Blue
#define LED_COLOR_YELLOW 0xFFFF00  // Yellow
#define LED_COLOR_CYAN   0x00FFFF  // Cyan

// LED system initialization and control functions
/**
 * @brief Initialize the LED control system
 * 
 * Initializes the LED driver, creates the LED task, and sets up the mutex
 * for thread-safe LED operations.
 */
void led_init(void);

/**
 * @brief Set LED color
 * 
 * This function sets the color of all 3 LEDs in a thread-safe manner.
 * The color should be in GRB format. Use led_color_lib functions
 * to generate colors (e.g., get_color_green_to_red()).
 * 
 * @param color Color value in GRB format (0x00RRGGBB)
 */
void led_set_color(uint32_t color);

/**
 * @brief Set LED intensity
 * 
 * This function sets the intensity (brightness) of all LEDs in a thread-safe manner.
 * The intensity value should be between 0.0 (off) and 1.0 (full brightness).
 * 
 * @param intensity Intensity value (0.0 to 1.0)
 */
void led_set_intensity(float intensity);

/**
 * @brief Get current LED color
 * 
 * This function returns the current LED color in a thread-safe manner.
 * 
 * @return Current color value in GRB format
 */
uint32_t led_get_color(void);

/**
 * @brief Get current LED intensity
 * 
 * This function returns the current LED intensity in a thread-safe manner.
 * 
 * @return Current intensity value (0.0 to 1.0)
 */
float led_get_intensity(void);

#endif // LED_H
