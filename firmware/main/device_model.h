//
// AirCube hardware model detection (Base vs Pro).
//
// A single firmware image runs on two hardware variants:
//   - Base: ENS210 (temp/RH) + ENS16X (air quality).
//   - Pro : SCD41 (true CO2 + temp/RH) + VCNL4040 (ambient light) + ENS16X,
//           with NO ENS210.
//
// The model is a property of the board, not a running measurement of sensor
// health, so it is resolved from a bus probe once and then latched in NVS.
// Later boots trust the latched value: a Pro whose SCD41 has died still reports
// Pro and raises a sensor fault, rather than quietly demoting itself to Base
// and reading temp/RH from an ENS210 that was never fitted.
//

#ifndef AIRCUBE_DEVICE_MODEL_H
#define AIRCUBE_DEVICE_MODEL_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    AIRCUBE_MODEL_BASE = 0,
    AIRCUBE_MODEL_PRO  = 1,
} aircube_model_t;

// Where the current model value came from.
typedef enum {
    AIRCUBE_MODEL_SOURCE_DEFAULT = 0,  // aircube_model_resolve() has not run
    AIRCUBE_MODEL_SOURCE_NVS     = 1,  // read back from the latch
    AIRCUBE_MODEL_SOURCE_PROBE   = 2,  // resolved from the bus and latched now
} aircube_model_source_t;

// Which optional sensors acknowledged on the bus during the last probe.
typedef struct {
    bool scd41;
    bool vcnl4040;
    bool ens210;
} aircube_model_probe_t;

// Probe the bus, then resolve and latch the model. Call once in app_main()
// AFTER nvs_flash_init() and i2c_driver_init(), and BEFORE any sensor init --
// the sensors to initialise are chosen from the result.
void aircube_model_resolve(void);

// Returns the resolved model. Defaults to BASE until aircube_model_resolve()
// has run.
aircube_model_t aircube_model_get(void);

// Convenience: true when the resolved model is Pro.
bool aircube_model_is_pro(void);

// Lowercase model name for logs / serial JSON ("base" or "pro").
const char *aircube_model_name(void);

// Where aircube_model_get() got its answer, and a short name for it.
aircube_model_source_t aircube_model_source(void);
const char *aircube_model_source_name(void);

// Results of the probe performed by aircube_model_resolve().
aircube_model_probe_t aircube_model_probe_result(void);

// True when a sensor the resolved model requires did not acknowledge. This is a
// hardware fault on a known-good model, not a reason to change the model.
bool aircube_model_sensor_missing(void);

// Erase the latch, re-probe and re-latch. For boards that have been reworked,
// and for units that latched the wrong model because a sensor was dead at the
// time of their first boot on latching firmware.
esp_err_t aircube_model_force_redetect(void);

#endif // AIRCUBE_DEVICE_MODEL_H
