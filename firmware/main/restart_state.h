#ifndef AIRCUBE_RESTART_STATE_H
#define AIRCUBE_RESTART_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t color;
    float intensity;
    float hue;
    uint16_t voc_level;
    uint16_t co2_level;
    bool auto_dim_night;
} restart_visual_state_t;

// Inspect retained RTC memory and the reset reason. Call once, early in app_main.
void restart_state_init(void);

// True when retained state is valid and the MCU reset without losing sensor power.
bool restart_state_is_warm_boot(void);

// Save/restore the live display state. RTC memory has no flash-wear cost.
void restart_state_update_visual(const restart_visual_state_t *state);
bool restart_state_get_visual(restart_visual_state_t *state);

// Tracks whether the SCD41 had been put into periodic measurement mode.
void restart_state_set_scd41_running(bool running);
bool restart_state_should_resume_scd41(void);

// Offset last written to the SCD41, in centi-degrees. Zero on a cold boot.
// A warm resume is only valid when this still matches the firmware offset.
uint16_t restart_state_get_scd41_offset_cdeg(void);
void restart_state_set_scd41_offset_cdeg(uint16_t offset_cdeg);

#endif // AIRCUBE_RESTART_STATE_H
