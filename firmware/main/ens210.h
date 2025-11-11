//
// Created by glina126 on 11/9/2023.
//

#ifndef DN680R_ENS210_H
#define DN680R_ENS210_H
#include <string.h>
#include <stdint.h>


void ens210_init(void);
float ens210_get_temperature(uint8_t type);
float ens210_get_humidity(void);
void ens210_get_envir(uint8_t * t, uint8_t * h);
void ens210_read_envir(void);
uint8_t ens210_get_status(void);
void ens210_deinit(void);

#endif //DN680R_ENS210_H
