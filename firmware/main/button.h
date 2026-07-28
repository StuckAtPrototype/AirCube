/**
 * @file button.h
 * @brief Button control header for brightness toggling
 * 
 * This header file defines the interface for button functionality to toggle
 * LED brightness levels. The button is on GPIO 11.
 * 
 * @author StuckAtPrototype, LLC
 * @version 1.0
 */

#ifndef BUTTON_H
#define BUTTON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize button functionality
 * 
 * This function configures GPIO 11 as an input with pull-down resistor
 * and sets up an interrupt on rising edge (button press). It also creates
 * a task to handle button debouncing and brightness toggling.
 * 
 * Brightness levels cycle through: 0.0 -> 0.1 -> 0.3 -> 0.6 -> 1.0 -> 0.0
 * Default brightness is 0.6.
 */
void button_init(void);

/**
 * @brief Set LED brightness to an arbitrary percent and persist it.
 *
 * Shared write path for remote brightness control (BLE characteristic,
 * Zigbee Analog Output cluster). Applies the intensity, saves it to NVS
 * so it survives reboots, and reports the change over Zigbee when joined.
 *
 * @param percent Brightness 0-100 (clamped)
 */
void button_set_brightness_percent(int percent);

/**
 * @brief Current LED brightness as percent (0-100).
 */
int button_get_brightness_percent(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_H

