//
// Serial Protocol Implementation
// JSON-based bidirectional communication over UART 0
//

#include "serial_protocol.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "led.h"
#include "history.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_protocol";

#define RX_BUF_SIZE 256
#define JSON_OUTPUT_BUF_SIZE 512
#define HISTORY_PAGE_BUF_SIZE 8192  // ~48 slots x ~150 bytes each
#define HISTORY_MAX_PAGE_SIZE 48    // Max slots per page request

// External function to get readout period (defined in main.c)
extern uint32_t get_sensor_readout_period_ms(void);
extern void set_sensor_readout_period_ms(uint32_t period);

void serial_protocol_init(void)
{
    // ESP32-H2 uses USB-Serial-JTAG for console (not UART 0)
    // Install the USB-Serial-JTAG driver for RX capability
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .rx_buffer_size = RX_BUF_SIZE * 2,
        .tx_buffer_size = RX_BUF_SIZE,
    };

    esp_err_t ret = usb_serial_jtag_driver_install(&usb_serial_config);
    if (ret == ESP_OK) {
        // Switch VFS to use the driver so RX data goes into the driver's buffer
        usb_serial_jtag_vfs_use_driver();
        ESP_LOGI(TAG, "USB-Serial-JTAG driver installed");
    } else {
        ESP_LOGW(TAG, "USB-Serial-JTAG driver install failed: %s (RX commands may not work)",
                 esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Serial protocol initialized (USB-Serial-JTAG)");
}

void serial_send_sensor_data(uint8_t ens210_status, float temperature_c, float humidity,
                             const char* ens16x_status_str, int etvoc, int eco2, int aqi, int aqi_uba)
{
    char json_buffer[JSON_OUTPUT_BUF_SIZE];
    
    // Get timestamp (milliseconds since boot)
    uint32_t timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Convert Celsius to Fahrenheit
    float temperature_f = temperature_c * 9.0f / 5.0f + 32.0f;
    
    // Format JSON output
    int len = snprintf(json_buffer, sizeof(json_buffer),
        "{\"ens210\":{\"status\":%u,\"temperature_c\":%.2f,\"temperature_f\":%.2f,\"humidity\":%.2f},"
        "\"ens16x\":{\"status\":\"%s\",\"etvoc\":%d,\"eco2\":%d,\"aqi\":%d,\"aqi_uba\":%d},"
        "\"timestamp\":%lu}\n",
        ens210_status, temperature_c, temperature_f, humidity,
        ens16x_status_str, etvoc, eco2, aqi, aqi_uba,
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

// ---------------------------------------------------------------------------
// History command handlers
// ---------------------------------------------------------------------------

void serial_send_history_info(void)
{
    uint16_t write_index, entry_count;
    history_get_info(&write_index, &entry_count);

    char response[128];
    int len = snprintf(response, sizeof(response),
        "{\"history_info\":{\"entries\":%u,\"capacity\":%u,\"slot_bytes\":%u,\"window_us\":%llu}}\n",
        entry_count, HISTORY_MAX_VALID_ENTRIES, HISTORY_SLOT_SIZE, (unsigned long long)HISTORY_WINDOW_US);

    if (len > 0 && len < (int)sizeof(response)) {
        printf("%s", response);
        fflush(stdout);
    }
}

void serial_send_history_page(uint16_t start, uint16_t count)
{
    uint16_t write_index, entry_count;
    history_get_info(&write_index, &entry_count);

    // Clamp request to valid range
    if (start >= entry_count) {
        send_error("start index out of range");
        return;
    }
    if (count > HISTORY_MAX_PAGE_SIZE) {
        count = HISTORY_MAX_PAGE_SIZE;
    }
    if (start + count > entry_count) {
        count = entry_count - start;
    }

    // Allocate buffer on stack for the JSON response
    char *buf = malloc(HISTORY_PAGE_BUF_SIZE);
    if (buf == NULL) {
        send_error("out of memory");
        return;
    }

    int pos = snprintf(buf, HISTORY_PAGE_BUF_SIZE,
        "{\"history\":[");

    for (uint16_t i = 0; i < count; i++) {
        history_slot_t slot;
        esp_err_t err = history_read_slot(start + i, &slot);

        if (i > 0) {
            pos += snprintf(buf + pos, HISTORY_PAGE_BUF_SIZE - pos, ",");
        }

        if (err == ESP_OK) {
            pos += snprintf(buf + pos, HISTORY_PAGE_BUF_SIZE - pos,
                "{\"seq\":%u,"
                "\"t_a\":%d,\"t_n\":%d,\"t_x\":%d,"
                "\"h_a\":%d,\"h_n\":%d,\"h_x\":%d,"
                "\"q_a\":%u,\"q_n\":%u,\"q_x\":%u,"
                "\"c_a\":%u,\"c_n\":%u,\"c_x\":%u,"
                "\"v_a\":%u,\"v_n\":%u,\"v_x\":%u}",
                slot.sequence,
                slot.temp_avg, slot.temp_min, slot.temp_max,
                slot.hum_avg, slot.hum_min, slot.hum_max,
                slot.aqi_avg, slot.aqi_min, slot.aqi_max,
                slot.eco2_avg, slot.eco2_min, slot.eco2_max,
                slot.etvoc_avg, slot.etvoc_min, slot.etvoc_max);
        } else {
            pos += snprintf(buf + pos, HISTORY_PAGE_BUF_SIZE - pos, "null");
        }

        if (pos >= HISTORY_PAGE_BUF_SIZE - 64) {
            // Buffer getting full, truncate
            break;
        }
    }

    pos += snprintf(buf + pos, HISTORY_PAGE_BUF_SIZE - pos,
        "],\"start\":%u,\"count\":%u}\n", start, count);

    if (pos > 0 && pos < HISTORY_PAGE_BUF_SIZE) {
        printf("%s", buf);
        fflush(stdout);
    }

    free(buf);
}

void serial_send_history_clear(void)
{
    esp_err_t err = history_clear();
    if (err == ESP_OK) {
        send_response("ok", "clear_history", 0);
    } else {
        send_error("failed to clear history");
    }
}

static void dump_history_csv(void)
{
    uint16_t write_index, entry_count;
    history_get_info(&write_index, &entry_count);

    printf("\n--- History: %u entries ---\n", entry_count);
    printf("slot,seq,temp_avg,temp_min,temp_max,hum_avg,hum_min,hum_max,"
           "aqi_avg,aqi_min,aqi_max,eco2_avg,eco2_min,eco2_max,"
           "etvoc_avg,etvoc_min,etvoc_max\n");

    for (uint16_t i = 0; i < entry_count; i++) {
        history_slot_t slot;
        if (history_read_slot(i, &slot) == ESP_OK) {
            printf("%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                   i, slot.sequence,
                   slot.temp_avg, slot.temp_min, slot.temp_max,
                   slot.hum_avg, slot.hum_min, slot.hum_max,
                   slot.aqi_avg, slot.aqi_min, slot.aqi_max,
                   slot.eco2_avg, slot.eco2_min, slot.eco2_max,
                   slot.etvoc_avg, slot.etvoc_min, slot.etvoc_max);
        }
    }
    printf("--- End ---\n");
    fflush(stdout);
}

static bool parse_command(const char* buffer, size_t len)
{
    // Quick shortcut: typing just "h" dumps history as CSV
    // Strip trailing \r if present (monitor sends \r\n)
    size_t slen = len;
    while (slen > 0 && (buffer[slen - 1] == '\r' || buffer[slen - 1] == '\n' || buffer[slen - 1] == ' ')) {
        slen--;
    }
    if (slen == 1 && buffer[0] == 'h') {
        dump_history_csv();
        return true;
    }

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
    
    // Handle get_history_info command
    if (strcmp(cmd_name, "get_history_info") == 0) {
        serial_send_history_info();
        return true;
    }
    
    // Handle clear_history command
    if (strcmp(cmd_name, "clear_history") == 0) {
        serial_send_history_clear();
        return true;
    }
    
    // Handle get_history command (with start and count params)
    if (strcmp(cmd_name, "get_history") == 0) {
        // Parse "start" field
        const char* start_str = strstr(buffer, "\"start\":");
        const char* count_str = strstr(buffer, "\"count\":");
        
        uint16_t start = 0;
        uint16_t count = HISTORY_MAX_PAGE_SIZE;
        
        if (start_str) {
            start = (uint16_t)atoi(start_str + 8);
        }
        if (count_str) {
            count = (uint16_t)atoi(count_str + 8);
        }
        
        serial_send_history_page(start, count);
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
    static uint8_t rx_buffer[RX_BUF_SIZE];
    static size_t buffer_pos = 0;
    
    // Read available data from USB-Serial-JTAG (non-blocking, 0 tick timeout)
    int len = usb_serial_jtag_read_bytes(rx_buffer + buffer_pos,
                                          RX_BUF_SIZE - buffer_pos - 1, 0);
    
    if (len > 0) {
        buffer_pos += len;
        rx_buffer[buffer_pos] = '\0'; // Null terminate
        
        // Single-character shortcuts (no Enter needed)
        if (buffer_pos == 1 && rx_buffer[0] == 'h') {
            dump_history_csv();
            buffer_pos = 0;
            return;
        }
        
        // Look for complete JSON commands (ending with \n or })
        char* newline = strchr((char*)rx_buffer, '\n');
        char* brace_end = strrchr((char*)rx_buffer, '}');
        
        if (newline || (brace_end && buffer_pos > 0)) {
            // Process command
            size_t cmd_len = newline ? (newline - (char*)rx_buffer) : 
                            (brace_end ? (brace_end - (char*)rx_buffer + 1) : buffer_pos);
            
            if (cmd_len > 0 && cmd_len < RX_BUF_SIZE) {
                rx_buffer[cmd_len] = '\0';
                parse_command((char*)rx_buffer, cmd_len);
            }
            
            // Shift remaining data to start of buffer
            size_t remaining = buffer_pos - cmd_len - (newline ? 1 : 0);
            if (remaining > 0 && remaining < RX_BUF_SIZE) {
                memmove(rx_buffer, rx_buffer + cmd_len + (newline ? 1 : 0), remaining);
                buffer_pos = remaining;
            } else {
                buffer_pos = 0;
            }
        }
        
        // Prevent buffer overflow
        if (buffer_pos >= RX_BUF_SIZE - 1) {
            ESP_LOGW(TAG, "Command buffer overflow, resetting");
            buffer_pos = 0;
        }
    }
}

