//
// Created by glina126 on 11/8/2023.
//

#ifndef DN680R_ENS16X_DRIVER_H
#define DN680R_ENS16X_DRIVER_H

#include <stdint.h>

enum ENS_STATUS{
    ENS_OP_OK = 0,
    ENS_WARM_UP,
    ENS_RESERVED,
    ENS_NO_VALID_OUTPUT
};

enum ENS_OPMODE{
    ENS_DEEP_SLEEP = 0,
    ENS_IDLE,
    ENS_STANDARD,
    ENS_LOW_POWER,
    ENS_ULTRA_LOW_POWER,
    ENS_RESET = 0xF0
};

void ens16x_init(void);

// Reads return -1 when the I2C transaction failed, so a bus problem cannot be
// mistaken for a real measurement of zero.
int ens16x_read_etvoc(void);
int ens16x_get_etvoc(void);
int ens16x_read_eco2(void);
int ens16x_read_aqi(void);
int ens16x_get_aqi(void);
int ens16x_read_aqi_uba(void);
int ens16x_get_aqi_uba(void);
enum ENS_STATUS ens16x_get_status(void);

// Refresh the cached status flags. Returns the raw status byte, or -1 if it
// could not be read (in which case the status becomes ENS_NO_VALID_OUTPUT).
int ens16x_get_device_status(void);
void ens16x_write_ens210_data(uint8_t * t, uint8_t * h);

#endif //DN680R_ENS16X_DRIVER_H
