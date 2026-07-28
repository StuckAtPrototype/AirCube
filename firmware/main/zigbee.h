/**
 * @file zigbee.h
 * @brief Zigbee integration for AirCube
 *
 * Exposes temperature, humidity, eCO2, eTVOC, and VOC Level over Zigbee
 * using standard ZCL clusters (temp/humidity), Basic cluster SWBuildID
 * for firmware version, a manufacturer-specific custom cluster (0xFC01)
 * for air quality metrics, and the standard Analog Output cluster (0x000D)
 * for LED brightness control.
 *
 * @author StuckAtPrototype, LLC
 */

#ifndef ZIGBEE_H
#define ZIGBEE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Zigbee stack and register the AirCube endpoint.
 *
 * Configures the ESP32-H2 as a Zigbee End Device with:
 *   - Temperature Measurement cluster (0x0402)
 *   - Relative Humidity cluster (0x0405)
 *   - Custom cluster (0xFC01) for eCO2, eTVOC, VOC Level (TVOC-derived)
 *   - Analog Output cluster (0x000D) for LED brightness
 *
 * On first boot (factory-new), the stack stays idle until the user
 * triggers pairing via a long button press.  On subsequent boots the
 * device reconnects to its previously-joined network automatically.
 * Must be called after nvs_flash_init().
 */
void zigbee_init(void);

/**
 * @brief Push latest sensor readings into Zigbee attributes.
 *
 * Call this from the sensor task after each readout cycle.
 * Values are only sent if the device has joined a Zigbee network.
 * The Zigbee stack handles attribute reporting automatically based
 * on configured min/max intervals and delta thresholds.
 *
 * @param temp_c    Temperature in degrees Celsius (after offset correction)
 * @param humidity  Relative humidity in percent (0-100)
 * @param eco2      Equivalent CO2 in ppm (ENS16X estimate, custom cluster 0xFC01)
 * @param etvoc     Equivalent TVOC in ppb
 * @param aqi       TVOC-derived VOC Level (0-500) on attribute 0x0002
 * @param co2_ppm   True CO2 in ppm (SCD41). Pro only; reported on the standard
 *                  Carbon Dioxide Measurement cluster (0x040D). Ignored on Base.
 * @param lux       Ambient light in lux (VCNL4040). Pro only; reported on the
 *                  standard Illuminance Measurement cluster (0x0400). Ignored on Base.
 */
void zigbee_update_sensors(float temp_c, float humidity, int eco2, int etvoc,
                           int aqi, float co2_ppm, float lux);

/**
 * @brief Push the current LED brightness to the Analog Output cluster.
 *
 * Call after a local brightness change (e.g. button press) so coordinators
 * see the update immediately instead of waiting for the next sensor cycle.
 */
void zigbee_report_brightness(void);

/**
 * @brief Check if the device has joined a Zigbee network.
 *
 * @return true if connected to a Zigbee coordinator, false otherwise
 */
bool zigbee_is_connected(void);

/**
 * @brief Trigger Zigbee network steering (pairing mode).
 *
 * Call this from a long button press. The device will scan all channels
 * and attempt to join an open Zigbee network. Pairing mode automatically
 * times out after 60 seconds if no network is found.
 *
 * Safe to call from any task context.
 */
void zigbee_start_pairing(void);

/**
 * @brief Check if the device is currently in pairing mode.
 *
 * Returns true from the moment zigbee_start_pairing() is called until
 * the device joins a network or the 60-second timeout expires.
 * Use this to drive a visual indicator (e.g., flashing blue LED).
 *
 * @return true if actively searching for a network, false otherwise
 */
bool zigbee_is_pairing(void);

#ifdef __cplusplus
}
#endif

#endif // ZIGBEE_H
