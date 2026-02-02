//
// Serial Protocol Header
// JSON-based bidirectional communication over UART 0
//

#ifndef SERIAL_PROTOCOL_H
#define SERIAL_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// Initialize serial protocol
void serial_protocol_init(void);

// Send sensor data as JSON
void serial_send_sensor_data(uint8_t ens210_status, float temperature_c, float humidity,
                             const char* ens16x_status_str, int etvoc, int eco2, int aqi);

// Process incoming commands (call periodically)
void serial_process_commands(void);

#endif // SERIAL_PROTOCOL_H

