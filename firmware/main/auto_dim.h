//
// Pro-only lux-based auto-dim for LED brightness.
// Base hardware always passes configured brightness through unchanged.
//

#ifndef AIRCUBE_AUTO_DIM_H
#define AIRCUBE_AUTO_DIM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool  enabled;
    float night_enter_lux;
    float day_exit_lux;
    int   night_dim_pct;
    int   lux_sample_count;
} auto_dim_config_t;

typedef struct {
    auto_dim_config_t config;
    bool  is_night;
    int   configured_pct;
    int   effective_pct;
    int   preset;
} auto_dim_status_t;

void auto_dim_init(void);

// Feed compensated ambient lux (Pro sensor loop). No-op on Base.
void auto_dim_update_lux(float lux);

// Recompute effective brightness from configured setting and apply to LED.
void auto_dim_recompute(void);

int  auto_dim_preset_from_percent(int percent);
int  auto_dim_get_effective_percent(void);
bool auto_dim_is_night(void);

void auto_dim_get_status(auto_dim_status_t *status);
void auto_dim_get_config(auto_dim_config_t *config);
void auto_dim_set_config(const auto_dim_config_t *config);

#endif // AIRCUBE_AUTO_DIM_H
