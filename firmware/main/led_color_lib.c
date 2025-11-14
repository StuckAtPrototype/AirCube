/**
 * @file led_color_lib.c
 * @brief LED color library implementation
 * 
 * This file implements various color generation and manipulation functions for the Racer3 device.
 * It provides hue-to-RGB conversion, color interpolation, pulsing effects, and full spectrum
 * color cycling capabilities for WS2812 LED control.
 * 
 * @author StuckAtPrototype, LLC
 * @version 3.0
 */

#include "led_color_lib.h"
#include <math.h>

// Static variables for color cycling and effects
static uint16_t hue_increment = 10;  // Increment for hue cycling
static uint16_t current_hue = 0;     // Current hue position

/**
 * @brief Convert hue to RGB values
 * 
 * This helper function converts a hue value (0.0 to 1.0) to RGB values using
 * the HSL color space model. It implements the standard hue-to-RGB conversion
 * algorithm for the six color sectors.
 * 
 * @param h Hue value (0.0 to 1.0, where 0=red, 1/6=yellow, 2/6=green, etc.)
 * @param r Pointer to store red component (0.0 to 1.0)
 * @param g Pointer to store green component (0.0 to 1.0)
 * @param b Pointer to store blue component (0.0 to 1.0)
 */
static void hue_to_rgb(float h, float *r, float *g, float *b) {
    // Calculate the intermediate value for color interpolation
    float x = 1 - fabsf(fmodf(h * 6, 2) - 1);

    // Convert hue to RGB based on the six color sectors
    if (h < 1.0f/6.0f)      { *r = 1; *g = x; *b = 0; }  // Red to Yellow
    else if (h < 2.0f/6.0f) { *r = x; *g = 1; *b = 0; }  // Yellow to Green
    else if (h < 3.0f/6.0f) { *r = 0; *g = 1; *b = x; }  // Green to Cyan
    else if (h < 4.0f/6.0f) { *r = 0; *g = x; *b = 1; }  // Cyan to Blue
    else if (h < 5.0f/6.0f) { *r = x; *g = 0; *b = 1; }  // Blue to Magenta
    else                    { *r = 1; *g = 0; *b = x; }  // Magenta to Red
}

/**
 * @brief Get RGB color from hue value
 * 
 * This function converts a 16-bit hue value to a 24-bit RGB color value
 * in GRB format suitable for WS2812 LEDs.
 * 
 * @param hue Hue value (0-65535, where 0=red, 10923=yellow, 21845=green, etc.)
 * @return 24-bit color value in GRB format
 */
uint32_t get_color_from_hue(uint16_t hue) {
    // Convert 16-bit hue to 0.0-1.0 range
    float h = hue / 65536.0f;
    float r, g, b;

    // Convert hue to RGB
    hue_to_rgb(h, &r, &g, &b);

    // Apply brightness limit and scale to 0-255 range
    r *= MAX_BRIGHTNESS * 255;
    g *= MAX_BRIGHTNESS * 255;
    b *= MAX_BRIGHTNESS * 255;

    // Convert to GRB format for WS2812 LEDs
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
}

/**
 * @brief Get next color in full spectrum cycle
 * 
 * This function returns the next color in a continuous spectrum cycle.
 * Each call advances the hue by the configured increment amount.
 * 
 * @return 24-bit color value in GRB format
 */
uint32_t get_next_color_full_spectrum(void) {
    uint32_t color = get_color_from_hue(current_hue);

    // Increment hue for next call
    current_hue += hue_increment;
    if (current_hue >= 65536) current_hue = 0;  // Wrap around

    return color;
}

/**
 * @brief Set the hue increment for color cycling
 * 
 * This function sets the step size for hue advancement in the full spectrum cycle.
 * Larger values result in faster color transitions.
 * 
 * @param increment Hue increment value (0-65535)
 */
void set_hue_increment(uint16_t increment) {
    hue_increment = increment;
}


/**
 * @brief Get color interpolated between blue and red
 * 
 * This function creates a color that interpolates between blue and red based on
 * the input value. It's useful for creating color gradients that represent
 * different states or values.
 * 
 * @param value Input value (should be between COLOR_BLUE_HUE and COLOR_RED_HUE)
 * @return 24-bit color value in GRB format
 */
uint32_t get_color_between_blue_red(float value) {
    float ratio;
    float r, g, b;

    // Ensure value is within the valid range
    if (value < COLOR_BLUE_HUE) value = COLOR_BLUE_HUE;
    if (value > COLOR_RED_HUE) value = COLOR_RED_HUE;

    // Calculate the ratio (0.0 for BLUE, 1.0 for RED)
    ratio = (value - COLOR_BLUE_HUE) / (COLOR_RED_HUE - COLOR_BLUE_HUE);

    // Interpolate between BLUE (0, 0, 1) and RED (1, 0, 0)
    r = ratio;
    g = 0.0f;
    b = 1.0f - ratio;

    // Apply brightness limit and scale to 0-255 range
    r *= MAX_BRIGHTNESS * 255;
    g *= MAX_BRIGHTNESS * 255;
    b *= MAX_BRIGHTNESS * 255;

    // Convert to GRB format for WS2812 LEDs
    return ((uint32_t)(g + 0.5f) << 16) | ((uint32_t)(r + 0.5f) << 8) | (uint32_t)(b + 0.5f);
}


/**
 * @brief Get color from green to red gradient
 * 
 * This function creates a smooth gradient from green to red based on
 * a step value. The gradient transitions through yellow in the middle.
 * 
 * @param step Step value (0-255, where 0 is green and 255 is red)
 * @return 24-bit color value in GRB format
 */
uint32_t get_color_green_to_red(uint8_t step) {
    float ratio = step / 255.0f;  // Convert to 0.0-1.0 range
    
    // Interpolate from green (0, 255, 0) to red (255, 0, 0) through yellow
    float r, g, b;
    
    if (ratio <= 0.5f) {
        // Green to Yellow: increase red, keep green at max
        r = ratio * 2.0f;  // 0.0 to 1.0
        g = 1.0f;           // Keep green at max
        b = 0.0f;
    } else {
        // Yellow to Red: keep red at max, decrease green
        r = 1.0f;           // Keep red at max
        g = 2.0f * (1.0f - ratio);  // 1.0 to 0.0
        b = 0.0f;
    }
    
    // Apply brightness limit and scale to 0-255 range
    r *= MAX_BRIGHTNESS * 255;
    g *= MAX_BRIGHTNESS * 255;
    b *= MAX_BRIGHTNESS * 255;
    
    // Convert to GRB format for WS2812 LEDs
    return ((uint32_t)(g + 0.5f) << 16) | ((uint32_t)(r + 0.5f) << 8) | (uint32_t)(b + 0.5f);
}

/**
 * @brief Apply intensity/brightness to a color
 * 
 * This function multiplies all color components by the intensity value
 * to create a dimming effect. The intensity should be between 0.0 (off)
 * and 1.0 (full brightness).
 * 
 * @param color Base color value in GRB format
 * @param intensity Intensity multiplier (0.0 to 1.0)
 * @return 24-bit color value in GRB format with applied intensity
 */
uint32_t apply_color_intensity(uint32_t color, float intensity) {
    // Clamp intensity to valid range
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    
    // Extract RGB components from GRB format
    uint8_t g = (color >> 16) & 0xFF;
    uint8_t r = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    // Apply intensity
    float r_scaled = r * intensity;
    float g_scaled = g * intensity;
    float b_scaled = b * intensity;
    
    // Convert back to GRB format
    return ((uint32_t)(g_scaled + 0.5f) << 16) | ((uint32_t)(r_scaled + 0.5f) << 8) | (uint32_t)(b_scaled + 0.5f);
}