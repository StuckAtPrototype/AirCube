//
// Shared I2C driver for ENS210 and ENS16X sensors
//

#ifndef I2C_DRIVER_H
#define I2C_DRIVER_H

#include <stdint.h>
#include "esp_err.h"

// Initialize I2C bus (call once at startup)
esp_err_t i2c_driver_init(void);

// Write data to I2C device
// device_addr: I2C device address
// data: buffer containing register address (first byte) followed by data bytes
// len: total length of data buffer (register address + data)
esp_err_t i2c_driver_write(uint8_t device_addr, const uint8_t *data, size_t len);

// Read data from I2C device
// device_addr: I2C device address
// reg_addr: register address to read from
// reg_len: length of register address (usually 1)
// data: buffer to store read data
// data_len: number of bytes to read
esp_err_t i2c_driver_read(uint8_t device_addr, const uint8_t *reg_addr, size_t reg_len, uint8_t *data, size_t data_len);

// Read raw bytes from I2C device with no register/command write phase.
// Used by sensors (e.g. SCD4x) that require a command write, a delay, then a
// separate read transaction rather than a combined write-read.
// device_addr: I2C device address
// data: buffer to store read data
// len: number of bytes to read
esp_err_t i2c_driver_read_raw(uint8_t device_addr, uint8_t *data, size_t len);

// Quietly probe whether a device acknowledges at the given address.
// Intended for presence detection of optional sensors: a missing device is an
// expected outcome, not a fault, so the low-level "i2c.master" NACK error logs
// are suppressed for the duration of the probe. Returns true if the device ACKs.
#include <stdbool.h>
bool i2c_driver_probe(uint8_t device_addr);

// Deinitialize I2C bus
void i2c_driver_deinit(void);

#endif // I2C_DRIVER_H

