/**
 * @file ble_gatt.h
 * @brief Connectable BLE GATT service for AirCube (BLE radio mode)
 *
 * Implements the protocol in docs/BLE_GATT_PROTOCOL.md:
 *   - Connectable advertising with a BTHome v2 service-data payload
 *     (Home Assistant Bluetooth proxies keep seeing live readings) and
 *     the AirCube 128-bit service UUID in the scan response.
 *   - Device Info characteristic (read): model, firmware, history geometry.
 *   - Live Data characteristic (read/notify): current sensor readings.
 *   - History Request (write) + History Data (notify): streaming history
 *     sync (no per-page round trips).
 *
 * Only initialized when the device boots in BLE radio mode (see
 * radio_mode.h). Never runs while the Zigbee stack is active.
 *
 * @author StuckAtPrototype, LLC
 */

#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start NimBLE, register the AirCube GATT service, begin advertising.
 *
 * Call once from app_main() when the boot radio mode is BLE.
 */
void ble_gatt_init(void);

/**
 * @brief Push the latest sensor readings.
 *
 * Updates the Live Data characteristic (notifying a subscribed client) and
 * refreshes the BTHome advertising payload. No-op if ble_gatt_init() was
 * never called.
 *
 * @param temp_c   Temperature in Celsius
 * @param humidity Relative humidity in percent
 * @param aqi      VOC Level (0-500)
 * @param eco2     Equivalent CO2 in ppm (ENS16X estimate)
 * @param etvoc    Equivalent TVOC in ppb
 * @param co2_ppm  True CO2 in ppm (0 on Base)
 * @param lux      Ambient light in lux (0 on Base)
 * @param aqi_uba  AQI-UBA index (1-5)
 */
void ble_gatt_update_live(float temp_c, float humidity, int aqi, int eco2,
                          int etvoc, uint16_t co2_ppm, float lux, int aqi_uba);

/** @return true if a BLE central is currently connected */
bool ble_gatt_is_connected(void);

/**
 * @brief Notify a subscribed BLE central that LED brightness changed.
 *
 * Call after a local brightness change (button press, Zigbee write) so a
 * connected app stays in sync. No-op when not initialized, not connected,
 * or the client has not subscribed to the Brightness characteristic.
 */
void ble_gatt_report_brightness(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_GATT_H
