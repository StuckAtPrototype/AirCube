//
// Serial Protocol Implementation
// JSON-based bidirectional communication over UART 0
//

#include "serial_protocol.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_protocol";

#define UART_NUM UART_NUM_0
#define UART_BUF_SIZE 256
#define JSON_OUTPUT_BUF_SIZE 512

// External function to get readout period (defined in main.c)
extern uint32_t get_sensor_readout_period_ms(void);
extern void set_sensor_readout_period_ms(uint32_t period);

void serial_protocol_init(void)
{
    // UART 0 is used by console
    // The console component installs the UART driver, but we need RX buffer for reading
    // Check if driver is already installed
    bool driver_installed = uart_is_driver_installed(UART_NUM);
    
    if (driver_installed) {
        // Driver already installed by console, we can use it for reading
        ESP_LOGI(TAG, "UART driver already installed by console, using existing driver");
    } else {
        // Install UART driver with RX buffer for command reading
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        
        uart_param_config(UART_NUM, &uart_config);
        // Install with RX buffer, small TX buffer (console may need it)
        esp_err_t ret = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 128, 0, NULL, 0);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "UART driver installed for reading");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            // Driver already installed, that's okay
            ESP_LOGI(TAG, "UART driver already installed");
        } else {
            ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "Serial protocol initialized on UART 0");
}

void serial_send_sensor_data(uint8_t ens210_status, float temperature_c, float humidity,
                             const char* ens16x_status_str, int etvoc, int eco2, int aqi)
{
    char json_buffer[JSON_OUTPUT_BUF_SIZE];
    
    // Get timestamp (milliseconds since boot)
    uint32_t timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Convert Celsius to Fahrenheit
    float temperature_f = temperature_c * 9.0f / 5.0f + 32.0f;
    
    // Format JSON output
    int len = snprintf(json_buffer, sizeof(json_buffer),
        "{\"ens210\":{\"status\":%u,\"temperature_c\":%.2f,\"temperature_f\":%.2f,\"humidity\":%.2f},"
        "\"ens16x\":{\"status\":\"%s\",\"etvoc\":%d,\"eco2\":%d,\"aqi\":%d},"
        "\"timestamp\":%lu}\n",
        ens210_status, temperature_c, temperature_f, humidity,
        ens16x_status_str, etvoc, eco2, aqi,
        (unsigned long)timestamp);
    
    if (len > 0 && len < sizeof(json_buffer)) {
        // Write to console UART using printf (goes to UART 0)
        printf("%s", json_buffer);
        fflush(stdout); // Ensure immediate output
    } else {
        ESP_LOGW(TAG, "JSON buffer too small or formatting error");
    }
}

static void send_response(const char* status, const char* cmd, float value)
{
    char response[128];
    int len = snprintf(response, sizeof(response),
        "{\"status\":\"%s\",\"cmd\":\"%s\",\"value\":%.2f}\n",
        status, cmd ? cmd : "", value);
    
    if (len > 0 && len < sizeof(response)) {
        printf("%s", response);
        fflush(stdout);
    }
}

static void send_error(const char* msg)
{
    char response[128];
    int len = snprintf(response, sizeof(response),
        "{\"status\":\"error\",\"msg\":\"%s\"}\n", msg);
    
    if (len > 0 && len < sizeof(response)) {
        printf("%s", response);
        fflush(stdout);
    }
}

static void send_config_response(float intensity, uint32_t period)
{
    char response[128];
    int len = snprintf(response, sizeof(response),
        "{\"config\":{\"intensity\":%.2f,\"readout_period\":%lu}}\n",
        intensity, (unsigned long)period);
    
    if (len > 0 && len < sizeof(response)) {
        printf("%s", response);
        fflush(stdout);
    }
}

static bool parse_command(const char* buffer, size_t len)
{
    // Simple JSON parsing for commands
    // Expected format: {"cmd":"command_name","value":number}
    // or {"cmd":"get_config"}
    
    if (len < 10) return false; // Minimum valid command size
    
    // Check if it starts with {"cmd":
    if (strncmp(buffer, "{\"cmd\":", 7) != 0) {
        return false;
    }
    
    // Find command name
    const char* cmd_start = strstr(buffer, "\"cmd\":\"");
    if (!cmd_start) return false;
    cmd_start += 7; // Skip "cmd":"
    
    const char* cmd_end = strchr(cmd_start, '"');
    if (!cmd_end) return false;
    
    size_t cmd_len = cmd_end - cmd_start;
    char cmd_name[32];
    if (cmd_len >= sizeof(cmd_name)) return false;
    strncpy(cmd_name, cmd_start, cmd_len);
    cmd_name[cmd_len] = '\0';
    
    // Handle get_config command (no value field needed)
    if (strcmp(cmd_name, "get_config") == 0) {
        float intensity = led_get_intensity();
        uint32_t period = get_sensor_readout_period_ms();
        send_config_response(intensity, period);
        return true;
    }
    
    // For set commands, find value field
    const char* value_start = strstr(buffer, "\"value\":");
    if (!value_start) {
        send_error("missing value field");
        return false;
    }
    value_start += 8; // Skip "value":
    
    // Parse float value
    float value = strtof(value_start, NULL);
    
    // Handle set_intensity command
    if (strcmp(cmd_name, "set_intensity") == 0) {
        // Clamp value to valid range
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        
        led_set_intensity(value);
        send_response("ok", "set_intensity", value);
        ESP_LOGI(TAG, "LED intensity set to %.2f", value);
        return true;
    }
    
    // Handle set_readout_period command
    if (strcmp(cmd_name, "set_readout_period") == 0) {
        // Clamp value to valid range (100ms to 10000ms)
        uint32_t period = (uint32_t)value;
        if (period < 100) period = 100;
        if (period > 10000) period = 10000;
        
        set_sensor_readout_period_ms(period);
        send_response("ok", "set_readout_period", (float)period);
        ESP_LOGI(TAG, "Sensor readout period set to %lu ms", (unsigned long)period);
        return true;
    }
    
    // Unknown command
    send_error("unknown command");
    return false;
}

void serial_process_commands(void)
{
    static uint8_t rx_buffer[UART_BUF_SIZE];
    static size_t buffer_pos = 0;
    
    // Check if driver is installed before trying to read
    if (!uart_is_driver_installed(UART_NUM)) {
        // Driver not available, skip reading
        return;
    }
    
    // Read available data from UART (non-blocking)
    int len = uart_read_bytes(UART_NUM, rx_buffer + buffer_pos, 
                              UART_BUF_SIZE - buffer_pos - 1, 0);
    
    if (len > 0) {
        buffer_pos += len;
        rx_buffer[buffer_pos] = '\0'; // Null terminate
        
        // Look for complete JSON commands (ending with \n or })
        char* newline = strchr((char*)rx_buffer, '\n');
        char* brace_end = strrchr((char*)rx_buffer, '}');
        
        if (newline || (brace_end && buffer_pos > 0)) {
            // Process command
            size_t cmd_len = newline ? (newline - (char*)rx_buffer) : 
                            (brace_end ? (brace_end - (char*)rx_buffer + 1) : buffer_pos);
            
            if (cmd_len > 0 && cmd_len < UART_BUF_SIZE) {
                rx_buffer[cmd_len] = '\0';
                parse_command((char*)rx_buffer, cmd_len);
            }
            
            // Shift remaining data to start of buffer
            size_t remaining = buffer_pos - cmd_len - (newline ? 1 : 0);
            if (remaining > 0 && remaining < UART_BUF_SIZE) {
                memmove(rx_buffer, rx_buffer + cmd_len + (newline ? 1 : 0), remaining);
                buffer_pos = remaining;
            } else {
                buffer_pos = 0;
            }
        }
        
        // Prevent buffer overflow
        if (buffer_pos >= UART_BUF_SIZE - 1) {
            ESP_LOGW(TAG, "Command buffer overflow, resetting");
            buffer_pos = 0;
        }
    }
}

