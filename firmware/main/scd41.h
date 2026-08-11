//
// Driver for the Sensirion SCD41 CO2 / temperature / humidity sensor.
// Used on the AirCube "Pro" hardware variant.
//

#ifndef AIRCUBE_SCD41_H
#define AIRCUBE_SCD41_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Bus address, exposed so the model detection can probe for this sensor without
// pulling in the driver.
#define SCD41_I2C_ADDRESS 0x62

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

// Cached values from the most recent accepted measurement. Only meaningful
// while scd41_has_data() is true.
uint16_t scd41_get_co2(void);            // CO2 concentration in ppm
float    scd41_get_temperature_c(void);  // temperature in degrees Celsius
float    scd41_get_humidity(void);       // relative humidity in %

// True only while the cached temperature/humidity can be trusted: at least one
// measurement has been accepted, it is recent, and no stuck-channel fault is
// latched. False during the initial ~5 s warm-up and again once readings go
// stale, so callers never publish an old value as if it were current.
bool scd41_has_data(void);

// True while the cached CO2 value can be trusted. Tracked separately from
// temperature/humidity because CO2 refreshes on its own, much slower cadence.
bool scd41_has_co2(void);

// Snapshot of driver health, for serial diagnostics and fault reporting.
typedef struct {
    bool     present;               // sensor ACKed at init
    bool     data_valid;            // same condition as scd41_has_data()
    bool     co2_valid;             // same condition as scd41_has_co2()
    bool     stuck_latched;         // identical T/RH words for too long
    uint32_t consecutive_failures;  // failed reads since the last good one
    uint32_t identical_streak;      // consecutive bit-identical T/RH pairs
    uint32_t stuck_events;          // times a stuck fault has latched
    uint32_t recovery_attempts;     // reinit attempts since boot
    uint32_t rejected_samples;      // reads dropped as implausible
    int64_t  last_good_age_ms;      // age of last accepted T/RH, -1 if never
    int64_t  last_co2_age_ms;       // age of last accepted CO2, -1 if never
} scd41_health_t;

void scd41_get_health(scd41_health_t *out);

// The most recent read_measurement response, kept for field triage: this is
// what identifies a sensor that is returning constant but CRC-valid data.
typedef struct {
    bool     valid;        // a frame has been captured since boot
    uint8_t  bytes[9];     // raw response, 3 words of [MSB, LSB, CRC]
    uint16_t words[3];     // decoded CO2, temperature, humidity words
    uint8_t  crc_mask;     // bit i set = word i passed its CRC
    int64_t  age_ms;       // how long ago the frame was read
} scd41_raw_frame_t;

bool scd41_get_raw_frame(scd41_raw_frame_t *out);

// Run the sensor's built-in self test (~10 s, blocking). On success *result is
// the raw status word, which is 0 when no malfunction is detected.
esp_err_t scd41_self_test(uint16_t *result);

// stop_periodic_measurement + reinit + re-apply the temperature offset, then
// resume the state machine. Clears the failure counters and any stuck fault so
// the sensor gets a fresh assessment. Never performs a factory reset.
esp_err_t scd41_reinit(void);

#endif // AIRCUBE_SCD41_H
