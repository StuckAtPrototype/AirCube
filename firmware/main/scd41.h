//
// Driver for the Sensirion SCD41 CO2 / temperature / humidity sensor.
// Used on the AirCube "Pro" hardware variant.
//

#ifndef AIRCUBE_SCD41_H
#define AIRCUBE_SCD41_H

#include <stdint.h>
#include <stdbool.h>

// Probe for the sensor, and if present configure it for single-shot operation
// (applies the static temperature offset and leaves the sensor idle).
// Safe to call on hardware that does not have the SCD41 fitted (Base model):
// it will simply mark the sensor as not present.
void scd41_init(void);

// Returns true if the SCD41 was detected during scd41_init().
bool scd41_is_present(void);

// Drives the single-shot measurement state machine. Call frequently (e.g. once
// per sensor-task loop). It triggers an RH+T single shot every ~5 s and a full
// CO2 single shot every ~30 s, reading the (multi-second) CO2 result
// asynchronously so it never blocks. Updates the cached getters.
// Returns true when fresh values were read on this call.
bool scd41_poll(void);

// Cached values from the most recent successful scd41_read().
uint16_t scd41_get_co2(void);            // CO2 concentration in ppm
float    scd41_get_temperature_c(void);  // temperature in degrees Celsius
float    scd41_get_humidity(void);       // relative humidity in %

// Returns true once the SCD41 has produced at least one valid temperature/RH
// measurement. False during the initial ~5 s warm-up (first single-shot), when
// the cached values are still 0 and must not be used for compensation.
bool scd41_has_data(void);

#endif // AIRCUBE_SCD41_H
