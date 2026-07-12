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
#include <stdbool.h>
#include "led.h"
#include "button.h"
#include "auto_dim.h"
#include "history.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_protocol";

#define RX_BUF_SIZE 256
#define JSON_OUTPUT_BUF_SIZE 512
// A single history slot can reach ~200 bytes once every numeric field is 4
// digits wide (seen in real logs, e.g. v_x=2286, c_a=1252). 48 slots * 200
// bytes plus the JSON envelope is ~9800 bytes, so keep a comfortable margin
// above the worst case to avoid any possibility of truncation.
#define HISTORY_PAGE_BUF_SIZE 12288
#define HISTORY_MAX_PAGE_SIZE 48    // Max slots per page request
// Largest slot footprint we will ever attempt to append. If adding another
// slot would push us past (HISTORY_PAGE_BUF_SIZE - HISTORY_SLOT_MAX_BYTES -
// HISTORY_FOOTER_BYTES) we stop early and report the actual count.
#define HISTORY_SLOT_MAX_BYTES 220
#define HISTORY_FOOTER_BYTES   64

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
                             const char* ens16x_status_str, int etvoc, int eco2,
                             int aqi, int aqi_s, int aqi_uba,
                             const char* model, int co2_ppm, float lux)
{
    char json_buffer[JSON_OUTPUT_BUF_SIZE];
    
    // Get timestamp (milliseconds since boot)
    uint32_t timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Convert Celsius to Fahrenheit
    float temperature_f = temperature_c * 9.0f / 5.0f + 32.0f;
    
    // Format JSON output. Note: "aqi" is the canonical AirCube VOC Level
    // (TVOC-derived, 0-500) starting in firmware 1.5.0. The previous AQI-S
    // value moved to "aqi_s" - this is a breaking change for any consumer
    // that was reading "aqi" as AQI-S in older firmware.
    // On Pro hardware temperature/humidity come from the SCD41 (the "ens210"
    // keys are kept for backward compatibility), and "scd41"/"vcnl4040" carry
    // the Pro-only true CO2 and ambient light. On Base co2/lux are 0.
    int len = snprintf(json_buffer, sizeof(json_buffer),
        "{\"model\":\"%s\","
        "\"ens210\":{\"status\":%u,\"temperature_c\":%.2f,\"temperature_f\":%.2f,\"humidity\":%.2f},"
        "\"ens16x\":{\"status\":\"%s\",\"etvoc\":%d,\"eco2\":%d,\"aqi\":%d,\"aqi_s\":%d,\"aqi_uba\":%d},"
        "\"scd41\":{\"co2\":%d},\"vcnl4040\":{\"lux\":%.1f},"
        "\"timestamp\":%lu}\n",
        model, ens210_status, temperature_c, temperature_f, humidity,
        ens16x_status_str, etvoc, eco2, aqi, aqi_s, aqi_uba,
        co2_ppm, lux,
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

static void send_config_response(void)
{
    auto_dim_status_t status;
    auto_dim_get_status(&status);

    char response[256];
    int len = snprintf(response, sizeof(response),
        "{\"config\":{\"intensity\":%.2f,\"readout_period\":%lu,"
        "\"auto_dim\":{\"enabled\":%s,\"night_enter_lux\":%.1f,\"day_exit_lux\":%.1f,"
        "\"night_dim_pct\":%d,\"is_night\":%s,\"configured_pct\":%d,\"effective_pct\":%d}}}\n",
        (float)status.configured_pct / 100.0f,
        (unsigned long)get_sensor_readout_period_ms(),
        status.config.enabled ? "true" : "false",
        status.config.night_enter_lux,
        status.config.day_exit_lux,
        status.config.night_dim_pct,
        status.is_night ? "true" : "false",
        status.configured_pct,
        status.effective_pct);

    if (len > 0 && len < (int)sizeof(response)) {
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
    // Note: %llu is unsupported by the default picolibc printf (snprintf
    // fails and the response is silently dropped); window_us fits in u32.
    int len = snprintf(response, sizeof(response),
        "{\"history_info\":{\"entries\":%u,\"capacity\":%u,\"slot_bytes\":%u,\"window_us\":%lu}}\n",
        entry_count, HISTORY_MAX_VALID_ENTRIES, HISTORY_SLOT_SIZE, (unsigned long)HISTORY_WINDOW_US);

    if (len > 0 && len < (int)sizeof(response)) {
        printf("%s", response);
        fflush(stdout);
    }
}

void serial_send_history_page(uint16_t start, uint16_t count)
{
    // One bulk history transfer at a time device-wide (shared with the BLE
    // streaming handler). Client retries after a short delay on "busy".
    if (!history_stream_acquire()) {
        send_error("busy");
        return;
    }

    uint16_t write_index, entry_count;
    history_get_info(&write_index, &entry_count);

    // Clamp request to valid range
    if (start >= entry_count) {
        send_error("start index out of range");
        history_stream_release();
        return;
    }
    if (count > HISTORY_MAX_PAGE_SIZE) {
        count = HISTORY_MAX_PAGE_SIZE;
    }
    if (start + count > entry_count) {
        count = entry_count - start;
    }

    // Allocate buffer on the heap for the JSON response
    char *buf = malloc(HISTORY_PAGE_BUF_SIZE);
    if (buf == NULL) {
        send_error("out of memory");
        history_stream_release();
        return;
    }

    // Tracks how many slots we actually serialized. We report this in the
    // response so the client doesn't advance past missing/truncated entries.
    uint16_t emitted = 0;
    size_t pos = 0;
    size_t saved_pos = 0;

    // safe_append: appends via snprintf while guaranteeing pos never exceeds
    // HISTORY_PAGE_BUF_SIZE - 1. Returns false if the write would overflow, in
    // which case pos is left unchanged so the caller can roll back.
    #define SAFE_APPEND(...) ({                                                \
        bool _ok = false;                                                      \
        if (pos < HISTORY_PAGE_BUF_SIZE) {                                     \
            size_t _remaining = HISTORY_PAGE_BUF_SIZE - pos;                   \
            int _written = snprintf(buf + pos, _remaining, __VA_ARGS__);       \
            if (_written > 0 && (size_t)_written < _remaining) {               \
                pos += (size_t)_written;                                       \
                _ok = true;                                                    \
            }                                                                  \
        }                                                                      \
        _ok;                                                                   \
    })

    if (!SAFE_APPEND("{\"history\":[")) {
        send_error("buffer overflow");
        free(buf);
        history_stream_release();
        return;
    }

    for (uint16_t i = 0; i < count; i++) {
        // Check up front whether there is enough room to fit another worst-
        // case slot plus the JSON footer. If not, stop cleanly; pos and
        // emitted reflect what we actually produced.
        if (pos + HISTORY_SLOT_MAX_BYTES + HISTORY_FOOTER_BYTES >= HISTORY_PAGE_BUF_SIZE) {
            break;
        }

        // Remember position in case we have to roll back a partial entry
        saved_pos = pos;

        if (i > 0) {
            if (!SAFE_APPEND(",")) {
                pos = saved_pos;
                break;
            }
        }

        history_slot_t slot;
        esp_err_t err = history_read_slot(start + i, &slot);
        bool ok;
        if (err == ESP_OK) {
            ok = SAFE_APPEND(
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
            ok = SAFE_APPEND("null");
        }

        if (!ok) {
            // Couldn't fit this entry – roll back the comma too
            pos = saved_pos;
            break;
        }

        emitted++;
    }

    // Closing footer: always report the actual number of emitted slots so the
    // client advances its cursor correctly even if we truncated.
    if (!SAFE_APPEND("],\"start\":%u,\"count\":%u}\n", start, emitted)) {
        // Should be impossible given the reservation above, but guard anyway
        ESP_LOGW(TAG, "history footer truncated (pos=%u)", (unsigned)pos);
        send_error("buffer overflow");
        free(buf);
        history_stream_release();
        return;
    }

    #undef SAFE_APPEND

    printf("%s", buf);
    fflush(stdout);

    free(buf);
    history_stream_release();
}

void serial_send_history_clear(void)
{
    esp_err_t err = history_clear();
    if (err == ESP_OK) {
        send_response("ok", "clear_history", 0);
    } else if (err == ESP_ERR_INVALID_STATE) {
        // A history stream is running (serial or BLE) - client retries later
        send_error("busy");
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
        send_config_response();
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
    
    // Handle set_auto_dim command (object value with optional fields)
    if (strcmp(cmd_name, "set_auto_dim") == 0) {
        auto_dim_config_t cfg;
        auto_dim_get_config(&cfg);

        const char *enabled_str = strstr(buffer, "\"enabled\":");
        if (enabled_str) {
            const char *val = enabled_str + 10;
            while (*val == ' ') val++;
            cfg.enabled = (strncmp(val, "true", 4) == 0 || strncmp(val, "1", 1) == 0);
        }

        const char *night_enter = strstr(buffer, "\"night_enter_lux\":");
        if (night_enter) {
            cfg.night_enter_lux = strtof(night_enter + 19, NULL);
        }

        const char *day_exit = strstr(buffer, "\"day_exit_lux\":");
        if (day_exit) {
            cfg.day_exit_lux = strtof(day_exit + 16, NULL);
        }

        const char *night_dim = strstr(buffer, "\"night_dim_pct\":");
        if (night_dim) {
            cfg.night_dim_pct = (int)strtol(night_dim + 17, NULL, 10);
        }

        const char *samples = strstr(buffer, "\"lux_sample_count\":");
        if (samples) {
            cfg.lux_sample_count = (int)strtol(samples + 20, NULL, 10);
        }

        auto_dim_set_config(&cfg);
        send_config_response();
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
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;

        int percent = (int)(value * 100.0f + 0.5f);
        button_set_brightness_percent(percent);
        send_response("ok", "set_intensity", value);
        ESP_LOGI(TAG, "LED intensity set to %.2f (%d%%)", value, percent);
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

