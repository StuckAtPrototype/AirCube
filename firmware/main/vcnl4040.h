//
// Driver for the Vishay VCNL4040 proximity + ambient light sensor.
// Used on the AirCube "Pro" hardware variant.
//

#ifndef AIRCUBE_VCNL4040_H
#define AIRCUBE_VCNL4040_H

#include <stdint.h>
#include <stdbool.h>

// Probe for the sensor (via its device ID register), and if present enable the
// ambient light (ALS) and proximity (PS) engines. Safe to call on hardware
// that does not have the VCNL4040 fitted (Base model).
void vcnl4040_init(void);

// Returns true if the VCNL4040 was detected during vcnl4040_init().
bool vcnl4040_is_present(void);

// Reads the latest proximity and ambient light values into the cached getters.
void vcnl4040_read(void);

uint16_t vcnl4040_get_proximity(void);   // raw proximity counts (PS_DATA)
uint16_t vcnl4040_get_ambient_raw(void); // raw ambient light counts (ALS_DATA)
float    vcnl4040_get_lux(void);         // ambient light in lux (calibrated, LED-compensated)

// Largest lux value vcnl4040_get_lux() can subtract as LED spill. Consumers that
// threshold on lux need to know this to stay stable when their own decision
// changes LED brightness.
float    vcnl4040_max_led_spill_lux(void);

#endif // AIRCUBE_VCNL4040_H
