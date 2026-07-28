/**
 * @file radio_mode.h
 * @brief BLE-first radio mode selection for AirCube
 *
 * The ESP32-H2 shares one radio between BLE and 802.15.4 (Zigbee), and
 * running both stacks concurrently caused PHY hangs. The device therefore
 * runs in exactly one radio mode, decided at boot:
 *
 *   - BLE (default): connectable GATT service + BTHome advertising.
 *     Active whenever the device is not commissioned on a Zigbee network.
 *   - Zigbee: active when the device has joined a network (or a pairing
 *     attempt was just requested).
 *
 * Transitions happen via NVS flags + reboot (no in-place stack teardown):
 *
 *   BLE -> Zigbee : long-press requests pairing; reboot into Zigbee mode
 *                   and run network steering.
 *   Zigbee -> BLE : network leave (hub removed us) or steering failure
 *                   while factory-new; flags cleared, reboot into BLE.
 *
 * @author StuckAtPrototype, LLC
 */

#ifndef RADIO_MODE_H
#define RADIO_MODE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_MODE_BLE = 0,
    RADIO_MODE_ZIGBEE,
} radio_mode_t;

/**
 * @brief Decide the boot radio mode from NVS flags.
 *
 * Must be called after nvs_flash_init() and before starting either stack.
 * Zigbee mode is chosen iff the "joined" flag is set (device previously
 * commissioned) or a pairing request flag is pending; otherwise BLE.
 */
void radio_mode_init(void);

/** @return the mode selected at boot (stable for the whole uptime) */
radio_mode_t radio_mode_get(void);

static inline bool radio_mode_is_zigbee_mode(void)
{
    return radio_mode_get() == RADIO_MODE_ZIGBEE;
}

/**
 * @brief Handle a long-press pairing request in the current mode.
 *
 * BLE mode:    set the pairing-request flag and reboot into Zigbee mode
 *              (steering starts automatically after reboot).
 * Zigbee mode: forward to zigbee_start_pairing() (re-pair to a new hub).
 */
void radio_mode_start_pairing(void);

/**
 * @brief Record that the device joined (or left) a Zigbee network.
 *
 * Called from the Zigbee signal handler. Persists the joined flag so the
 * next boot picks Zigbee mode.
 */
void radio_mode_set_joined(bool joined);

/**
 * @brief Leave Zigbee mode: clear flags and reboot into BLE mode.
 *
 * Called when the hub removes the device (network leave without rejoin)
 * or when a pairing attempt times out without joining.
 */
void radio_mode_revert_to_ble(void);

#ifdef __cplusplus
}
#endif

#endif // RADIO_MODE_H
