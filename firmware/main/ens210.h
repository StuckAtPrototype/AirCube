//
// Created by glina126 on 11/9/2023.
//

#ifndef DN680R_ENS210_H
#define DN680R_ENS210_H
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Bus address, exposed so the model detection can probe for this sensor without
// pulling in the driver.
#define ENS210_I2C_ADDRESS 0x43

void ens210_init(void);
float ens210_get_temperature(uint8_t type);
float ens210_get_humidity(void);
void ens210_get_envir(uint8_t * t, uint8_t * h);
void ens210_read_envir(void);
uint8_t ens210_get_status(void);
bool ens210_is_present(void);

// Whether the most recent ens210_read_envir() actually produced each value.
// False means the cached getter still holds an older reading.
bool ens210_temperature_valid(void);
bool ens210_humidity_valid(void);
void ens210_deinit(void);

#endif //DN680R_ENS210_H
