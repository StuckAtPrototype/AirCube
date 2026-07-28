//
// AirCube hardware model detection (Base vs Pro).
//
// A single firmware image runs on two hardware variants:
//   - Base: ENS210 (temp/RH) + ENS16X (air quality).
//   - Pro : SCD41 (true CO2 + temp/RH) + VCNL4040 (ambient light) + ENS16X,
//           with NO ENS210.
//
// The model is detected once at boot from the sensors that the drivers
// already probe for, and then queried throughout the firmware to route data
// and conditionally expose Pro-only features.
//

#ifndef AIRCUBE_DEVICE_MODEL_H
#define AIRCUBE_DEVICE_MODEL_H

#include <stdbool.h>

typedef enum {
    AIRCUBE_MODEL_BASE = 0,
    AIRCUBE_MODEL_PRO  = 1,
} aircube_model_t;

// Detect the hardware model from sensor presence. Call once in app_main()
// AFTER scd41_init() and vcnl4040_init() have run (they perform the probing).
void aircube_model_detect(void);

// Returns the detected model. Defaults to BASE until aircube_model_detect()
// has run.
aircube_model_t aircube_model_get(void);

// Convenience: true when the detected model is Pro.
bool aircube_model_is_pro(void);

// Lowercase model name for logs / serial JSON ("base" or "pro").
const char *aircube_model_name(void);

#endif // AIRCUBE_DEVICE_MODEL_H
