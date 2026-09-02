//
// Serial Protocol Implementation
// JSON-based bidirectional communication over UART 0
//

#include "serial_protocol.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "led.h"
#include "button.h"
#include "auto_dim.h"
#include "history.h"
#include "device_model.h"
#include "scd41.h"
#include "ens210.h"
#include "ens16x_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdarg.h>

static const char *TAG = "serial_protocol";

#define RX_BUF_SIZE 256
// TX ring the driver drains into the USB FIFO. Only needs to be big enough that
// the writer isn't woken for every packet; a history page is many times this.
#define TX_BUF_SIZE 2048
// Per-chunk ceiling on how long we wait for the host to drain the ring. Long
// enough to ride out a browser hiccup. This is only ever paid once per stall:
// after a chunk times out we assume nobody is reading and stop waiting (see
// s_host_stalled), because paying it on every log line while the port is
// closed stretched the sensor loop from 1 s to 3.2 s and made every button
// press take a second to land.
#define TX_CHUNK_TIMEOUT_MS 200
// Must stay comfortably under TX_BUF_SIZE: the driver's write is all-or-nothing
// against the ring, so an oversized request can never be satisfied.
#define TX_CHUNK_BYTES 512
#define LOG_LINE_BUF_SIZE 256
// Reads per poll. The RX ring is larger than one read, so a burst needs several
// passes to clear; the cap keeps the command task from spinning forever.
#define RX_PASSES_PER_POLL 8
#define JSON_OUTPUT_BUF_SIZE 768
#define HEALTH_JSON_BUF_SIZE 640
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

// Serialises every byte we put on the wire. Both protocol frames and log lines
// take it, so a log emitted mid-transfer cannot land inside a history page.
static SemaphoreHandle_t s_tx_lock;
static bool s_tx_direct;
// Set when a chunk write timed out because the ring stayed full: the port is
// closed on the host, or the cable goes to a charger. The USB peripheral keeps
// enumerating either way, so the ring is the only reliable signal. While set,
// writes are attempted without waiting and dropped if the ring is still full;
// the flag clears as soon as a write goes through (host started draining) or
// the host sends us anything (it is obviously listening).
static volatile bool s_host_stalled;

/*
 * Write straight to the USB-Serial-JTAG driver rather than through stdout.
 *
 * stdout reaches the host by a route that is both slow and lossy: the primary
 * console is UART0, whose blocking write paces every byte at 115200 even though
 * nothing is attached to it, and the USB copy is a secondary console sink that
 * pushes one byte at a time and silently discards the rest once its ring stays
 * full for 50 ms. Its return value is thrown away by the console layer, so the
 * drop is invisible. The only reason that sink survives today is that the UART
 * throttles the producer below the rate at which it can back up - which is why
 * simply raising the console baud rate corrupts the stream instead of speeding
 * it up.
 *
 * The driver call below copies into a ring the ISR drains, blocks while that
 * ring is full, and reports what it took, so a slow host slows us down instead
 * of losing bytes. A host that is not reading at all is a different matter:
 * once we detect that, output is dropped rather than waited on (s_host_stalled).
 */
/*
 * Push a buffer out in ring-sized bites. Caller must hold s_tx_lock.
 *
 * usb_serial_jtag_write_bytes() is all-or-nothing: it hands the whole request
 * to xRingbufferSend(), which waits for that many contiguous bytes to come
 * free and otherwise fails. A request larger than the TX ring can therefore
 * never succeed, so a history page has to be split rather than handed over
 * whole.
 */
static void tx_locked(const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > TX_CHUNK_BYTES) {
            chunk = TX_CHUNK_BYTES;
        }
        TickType_t wait = s_host_stalled ? 0 : pdMS_TO_TICKS(TX_CHUNK_TIMEOUT_MS);
        int n = usb_serial_jtag_write_bytes(data + sent, chunk, wait);
        if (n <= 0) {
            // Host has stopped reading. Abandon the rest of this frame rather
            // than block; the client re-requests what it never got, and every
            // write until the host comes back is non-blocking.
            s_host_stalled = true;
            break;
        }
        s_host_stalled = false;
        sent += (size_t)n;
    }
}

static void serial_write(const char *data, size_t len)
{
    if (!s_tx_direct) {
        // Pre-init, or the driver refused to install: stdout is all we have.
        fwrite(data, 1, len, stdout);
        fflush(stdout);
        return;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    tx_locked(data, len);
    xSemaphoreGive(s_tx_lock);
}

static void serial_write_str(const char *s)
{
    serial_write(s, strlen(s));
}

/*
 * Route ESP_LOG through the same lock, so log output cannot interleave into a
 * frame. It also keeps logs visible to `idf.py monitor`, which reads the USB
 * port, now that protocol output no longer travels via the console.
 */
static int serial_log_vprintf(const char *fmt, va_list args)
{
    if (!s_tx_direct || xPortInIsrContext()) {
        return vprintf(fmt, args);
    }

    // Static rather than stack: this runs on whichever task logged, and some of
    // them have small stacks. The lock inside serial_write does not cover the
    // formatting, so take it here for the buffer's sake as well.
    static char line[LOG_LINE_BUF_SIZE];

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    if (len > 0) {
        tx_locked(line, (len < (int)sizeof(line)) ? (size_t)len : sizeof(line) - 1);
    }
    xSemaphoreGive(s_tx_lock);
    return len;
}

void serial_protocol_init(void)
{
    // ESP32-H2 uses USB-Serial-JTAG for console (not UART 0)
    // Install the USB-Serial-JTAG driver for RX capability
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .rx_buffer_size = RX_BUF_SIZE * 2,
        .tx_buffer_size = TX_BUF_SIZE,
    };

    s_tx_lock = xSemaphoreCreateMutex();

    esp_err_t ret = usb_serial_jtag_driver_install(&usb_serial_config);
    if (ret == ESP_OK && s_tx_lock != NULL) {
        // Switch VFS to use the driver so RX data goes into the driver's buffer
        usb_serial_jtag_vfs_use_driver();
        s_tx_direct = true;
        esp_log_set_vprintf(serial_log_vprintf);
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
    // Static rather than on the stack: this is called from the sensor task once
    // a second and the frame is large enough to matter against a 4 KB stack.
    // Single-caller by design (sensor_task); not safe to call concurrently.
    static char json_buffer[JSON_OUTPUT_BUF_SIZE];

    // Get timestamp (milliseconds since boot)
    uint32_t timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Convert Celsius to Fahrenheit
    float temperature_f = temperature_c * 9.0f / 5.0f + 32.0f;
    
    // Format JSON output. Note: "aqi" is the canonical AirCube VOC Level
    // (TVOC-derived, 0-500). "aqi_s" is deprecated and always 0.
    // On Pro hardware temperature/humidity come from the SCD41 (the "ens210"
    // keys are kept for backward compatibility), and "scd41"/"vcnl4040" carry
    // the Pro-only true CO2 and ambient light. On Base co2/lux are 0.
    // Compact per-channel validity, additive so the Windows app is unaffected.
    // A consumer that reads these can tell a current reading from a held one;
    // the full picture is available from the get_sensor_health command.
    bool is_pro = aircube_model_is_pro();
    bool temp_valid = is_pro ? scd41_has_data() : ens210_temperature_valid();
    bool hum_valid  = is_pro ? scd41_has_data() : ens210_humidity_valid();
    bool co2_valid  = is_pro ? scd41_has_co2() : (eco2 >= 0);

    // esp_app_desc_t.version is a fixed 32-byte field that is not guaranteed to
    // be terminated, so bound the conversion rather than trusting a NUL.
    int len = snprintf(json_buffer, sizeof(json_buffer),
        "{\"model\":\"%s\",\"fw\":\"%.32s\","
        "\"ens210\":{\"status\":%u,\"temperature_c\":%.2f,\"temperature_f\":%.2f,\"humidity\":%.2f},"
        "\"ens16x\":{\"status\":\"%s\",\"etvoc\":%d,\"eco2\":%d,\"aqi\":%d,\"aqi_s\":%d,\"aqi_uba\":%d},"
        "\"scd41\":{\"co2\":%d},\"vcnl4040\":{\"lux\":%.1f},"
        "\"health\":{\"ok\":%s,\"temp_valid\":%s,\"hum_valid\":%s,\"co2_valid\":%s,"
        "\"etvoc_valid\":%s,\"sensor_missing\":%s,\"frc_needed\":%s},"
        "\"timestamp\":%lu}\n",
        model, esp_app_get_description()->version,
        ens210_status, temperature_c, temperature_f, humidity,
        ens16x_status_str, etvoc, eco2, aqi, aqi_s, aqi_uba,
        co2_ppm, lux,
        (temp_valid && hum_valid && etvoc >= 0 && (!is_pro || co2_valid)) ? "true" : "false",
        temp_valid ? "true" : "false",
        hum_valid ? "true" : "false",
        co2_valid ? "true" : "false",
        (etvoc >= 0) ? "true" : "false",
        aircube_model_sensor_missing() ? "true" : "false",
        scd41_frc_needed() ? "true" : "false",
        (unsigned long)timestamp);
    
    if (len > 0 && len < sizeof(json_buffer)) {
        serial_write(json_buffer, (size_t)len);
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
        serial_write(response, (size_t)len);
    }
}

static void send_error(const char* msg)
{
    char response[128];
    int len = snprintf(response, sizeof(response),
        "{\"status\":\"error\",\"msg\":\"%s\"}\n", msg);
    
    if (len > 0 && len < sizeof(response)) {
        serial_write(response, (size_t)len);
    }
}

// Per-sensor health, emitted both as an additive key on the periodic sensor
// frame and on demand via get_sensor_health. Additive keys need no Windows app
// change; the app ignores fields it does not know about.
static int format_health_json(char *buf, size_t size)
{
    aircube_model_probe_t probe = aircube_model_probe_result();
    bool is_pro = aircube_model_is_pro();

    scd41_health_t scd = {0};
    scd41_get_health(&scd);

    bool temp_valid = is_pro ? scd.data_valid : ens210_temperature_valid();
    bool hum_valid  = is_pro ? scd.data_valid : ens210_humidity_valid();

    return snprintf(buf, size,
        "{\"model\":\"%s\",\"model_source\":\"%s\",\"sensor_missing\":%s,"
        "\"temp_valid\":%s,\"hum_valid\":%s,"
        "\"probe\":{\"scd41\":%d,\"vcnl4040\":%d,\"ens210\":%d},"
        "\"scd41\":{\"present\":%s,\"valid\":%s,\"co2_valid\":%s,\"stuck\":%s,"
        "\"fails\":%lu,\"identical\":%lu,\"stuck_events\":%lu,"
        "\"recoveries\":%lu,\"rejected\":%lu,\"co2_fails\":%lu,\"self_test\":%ld,"
        "\"age_ms\":%lld,\"co2_age_ms\":%lld},"
        "\"ens16x\":{\"etvoc_valid\":%s},\"frc_needed\":%s}",
        aircube_model_name(), aircube_model_source_name(),
        aircube_model_sensor_missing() ? "true" : "false",
        temp_valid ? "true" : "false",
        hum_valid ? "true" : "false",
        probe.scd41, probe.vcnl4040, probe.ens210,
        scd.present ? "true" : "false",
        scd.data_valid ? "true" : "false",
        scd.co2_valid ? "true" : "false",
        scd.stuck_latched ? "true" : "false",
        (unsigned long)scd.consecutive_failures,
        (unsigned long)scd.identical_streak,
        (unsigned long)scd.stuck_events,
        (unsigned long)scd.recovery_attempts,
        (unsigned long)scd.rejected_samples,
        (unsigned long)scd.co2_failures,
        (long)scd.self_test_result,
        (long long)scd.last_good_age_ms,
        (long long)scd.last_co2_age_ms,
        (ens16x_get_etvoc() >= 0) ? "true" : "false",
        scd41_frc_needed() ? "true" : "false");
}

static void send_model_response(const char *cmd)
{
    aircube_model_probe_t probe = aircube_model_probe_result();

    char response[224];
    int len = snprintf(response, sizeof(response),
        "{\"status\":\"ok\",\"cmd\":\"%s\",\"model\":\"%s\",\"source\":\"%s\","
        "\"sensor_missing\":%s,"
        "\"probe\":{\"scd41\":%d,\"vcnl4040\":%d,\"ens210\":%d}}\n",
        cmd, aircube_model_name(), aircube_model_source_name(),
        aircube_model_sensor_missing() ? "true" : "false",
        probe.scd41, probe.vcnl4040, probe.ens210);

    if (len > 0 && len < (int)sizeof(response)) {
        serial_write(response, (size_t)len);
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
        serial_write(response, (size_t)len);
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
        serial_write(response, (size_t)len);
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

    serial_write(buf, pos);

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

    char line[192];
    int len = snprintf(line, sizeof(line), "\n--- History: %u entries ---\n", entry_count);
    serial_write(line, (size_t)len);
    serial_write_str("slot,seq,temp_avg,temp_min,temp_max,hum_avg,hum_min,hum_max,"
                     "aqi_avg,aqi_min,aqi_max,eco2_avg,eco2_min,eco2_max,"
                     "etvoc_avg,etvoc_min,etvoc_max\n");

    for (uint16_t i = 0; i < entry_count; i++) {
        history_slot_t slot;
        if (history_read_slot(i, &slot) == ESP_OK) {
            len = snprintf(line, sizeof(line),
                   "%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                   i, slot.sequence,
                   slot.temp_avg, slot.temp_min, slot.temp_max,
                   slot.hum_avg, slot.hum_min, slot.hum_max,
                   slot.aqi_avg, slot.aqi_min, slot.aqi_max,
                   slot.eco2_avg, slot.eco2_min, slot.eco2_max,
                   slot.etvoc_avg, slot.etvoc_min, slot.etvoc_max);
            if (len > 0 && len < (int)sizeof(line)) {
                serial_write(line, (size_t)len);
            }
        }
    }
    serial_write_str("--- End ---\n");
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
    
    // Report the latched model without changing it
    if (strcmp(cmd_name, "get_model") == 0) {
        send_model_response("get_model");
        return true;
    }

    // Clear the NVS model latch and re-resolve from a fresh bus probe. The
    // sensors for the old model are already initialised, so a reboot is needed
    // for the new one to take effect.
    if (strcmp(cmd_name, "redetect_model") == 0) {
        if (aircube_model_force_redetect() == ESP_OK) {
            send_model_response("redetect_model");
        } else {
            send_error("redetect_model failed");
        }
        return true;
    }

    // Full per-sensor health for field triage
    if (strcmp(cmd_name, "get_sensor_health") == 0) {
        // Static for the same reason as json_buffer: large frames stay off the
        // task stack. The command task is the only caller.
        static char health[HEALTH_JSON_BUF_SIZE];
        int hlen = format_health_json(health, sizeof(health) - 1);
        if (hlen > 0 && hlen < (int)sizeof(health) - 1) {
            health[hlen] = '\n';
            serial_write(health, (size_t)hlen + 1);
        } else {
            send_error("health buffer too small");
        }
        return true;
    }

    // Raw read_measurement frame. This is what distinguishes a sensor returning
    // constant-but-CRC-valid data from a decode problem on our side.
    if (strcmp(cmd_name, "scd41_raw") == 0) {
        scd41_raw_frame_t frame;
        if (!scd41_get_raw_frame(&frame)) {
            send_error("no scd41 frame captured yet");
            return true;
        }
        char raw[256];
        int rlen = snprintf(raw, sizeof(raw),
               "{\"status\":\"ok\",\"cmd\":\"scd41_raw\",\"age_ms\":%lld,"
               "\"bytes\":\"%02X %02X %02X %02X %02X %02X %02X %02X %02X\","
               "\"co2_word\":\"0x%04X\",\"t_word\":\"0x%04X\",\"rh_word\":\"0x%04X\","
               "\"crc_ok\":{\"co2\":%s,\"temp\":%s,\"hum\":%s}}\n",
               (long long)frame.age_ms,
               frame.bytes[0], frame.bytes[1], frame.bytes[2],
               frame.bytes[3], frame.bytes[4], frame.bytes[5],
               frame.bytes[6], frame.bytes[7], frame.bytes[8],
               frame.words[0], frame.words[1], frame.words[2],
               (frame.crc_mask & 0x01) ? "true" : "false",
               (frame.crc_mask & 0x02) ? "true" : "false",
               (frame.crc_mask & 0x04) ? "true" : "false");
        if (rlen > 0 && rlen < (int)sizeof(raw)) {
            serial_write(raw, (size_t)rlen);
        }
        return true;
    }

    // Sensor built-in self test. Blocks for about 10 seconds.
    if (strcmp(cmd_name, "scd41_selftest") == 0) {
        uint16_t result = 0;
        esp_err_t err = scd41_self_test(&result);
        if (err == ESP_OK) {
            char reply[128];
            int slen = snprintf(reply, sizeof(reply),
                   "{\"status\":\"ok\",\"cmd\":\"scd41_selftest\",\"result\":\"0x%04X\","
                   "\"malfunction\":%s}\n",
                   result, (result == 0) ? "false" : "true");
            if (slen > 0 && slen < (int)sizeof(reply)) {
                serial_write(reply, (size_t)slen);
            }
        } else {
            send_error(esp_err_to_name(err));
        }
        return true;
    }

    // Manual stop_periodic + reinit. Never a factory reset.
    if (strcmp(cmd_name, "scd41_reinit") == 0) {
        esp_err_t err = scd41_reinit();
        if (err == ESP_OK) {
            send_response("ok", "scd41_reinit", 0);
        } else {
            send_error(esp_err_to_name(err));
        }
        return true;
    }

    // Dismiss the one-time post-upgrade FRC prompt without calibrating.
    if (strcmp(cmd_name, "scd41_frc_ack") == 0) {
        scd41_clear_frc_needed();
        send_response("ok", "scd41_frc_ack", 0);
        return true;
    }

    // Forced recalibration to outdoor background CO2. The host is responsible
    // for making sure the cube has been sitting in fresh air long enough first.
    if (strcmp(cmd_name, "scd41_frc") == 0) {
        int16_t correction = 0;
        esp_err_t err = scd41_forced_recalibration(&correction);
        char reply[128];
        int rlen;
        if (err == ESP_OK) {
            rlen = snprintf(reply, sizeof(reply),
                   "{\"status\":\"ok\",\"cmd\":\"scd41_frc\",\"target\":%d,"
                   "\"correction\":%d}\n", SCD41_FRC_TARGET_PPM, (int)correction);
        } else {
            rlen = snprintf(reply, sizeof(reply),
                   "{\"status\":\"error\",\"cmd\":\"scd41_frc\",\"msg\":\"%s\"}\n",
                   esp_err_to_name(err));
        }
        if (rlen > 0 && rlen < (int)sizeof(reply)) {
            serial_write(reply, (size_t)rlen);
        }
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

    // Bound the work per poll so a host that streams without pause cannot keep
    // this task from yielding.
    for (int pass = 0; pass < RX_PASSES_PER_POLL; pass++) {
        // A buffer this full holds no terminator, so nothing in it will ever
        // parse. Reset before reading rather than after: at RX_BUF_SIZE - 1 the
        // read below asks for zero bytes and returns 0 without touching the
        // buffer, so recovery placed after it can never run and the command
        // path stays dead until reboot.
        if (buffer_pos >= RX_BUF_SIZE - 1) {
            ESP_LOGW(TAG, "Command buffer overflow, resetting");
            buffer_pos = 0;
        }

        int len = usb_serial_jtag_read_bytes(rx_buffer + buffer_pos,
                                             RX_BUF_SIZE - buffer_pos - 1, 0);
        if (len <= 0) {
            return;
        }

        // A host that talks to us is reading too, even if the ring still holds
        // stale output from before it opened the port: let the reply wait for
        // it to drain rather than dropping it on the floor.
        s_host_stalled = false;

        buffer_pos += (size_t)len;
        rx_buffer[buffer_pos] = '\0';

        // Single-character shortcut (no Enter needed). Only reachable at a
        // command boundary, since every JSON command opens with '{'.
        if (buffer_pos == 1 && rx_buffer[0] == 'h') {
            dump_history_csv();
            buffer_pos = 0;
            continue;
        }

        // Drain every complete command the read delivered, not just the first.
        // Bursts arrive far faster than this poll runs - a brightness slider
        // sends one command per pointer move - and leaving the remainder queued
        // is what let the buffer fill up in the first place.
        for (;;) {
            char *newline = memchr(rx_buffer, '\n', buffer_pos);
            size_t cmd_len;
            size_t consumed;

            if (newline != NULL) {
                cmd_len = (size_t)(newline - (char *)rx_buffer);
                consumed = cmd_len + 1;
            } else if (buffer_pos > 0 && rx_buffer[buffer_pos - 1] == '}') {
                // A command pasted into a terminal without pressing Enter.
                cmd_len = buffer_pos;
                consumed = buffer_pos;
            } else {
                break;  // Partial command, wait for the rest.
            }

            if (cmd_len > 0) {
                rx_buffer[cmd_len] = '\0';
                parse_command((char *)rx_buffer, cmd_len);
            }

            buffer_pos -= consumed;
            if (buffer_pos > 0) {
                memmove(rx_buffer, rx_buffer + consumed, buffer_pos);
            }
            rx_buffer[buffer_pos] = '\0';
        }
    }
}

