//
// History Module Header
// Append-only ring buffer for 7-day sensor history on flash partition
//

#ifndef HISTORY_H
#define HISTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Ring buffer geometry
#define HISTORY_SECTOR_SIZE       4096
#define HISTORY_SLOT_SIZE         32
#define HISTORY_SLOTS_PER_SECTOR  (HISTORY_SECTOR_SIZE / HISTORY_SLOT_SIZE)  // 128
#define HISTORY_NUM_SECTORS       17
#define HISTORY_TOTAL_SLOTS       (HISTORY_NUM_SECTORS * HISTORY_SLOTS_PER_SECTOR)  // 2176
#define HISTORY_MAX_VALID_ENTRIES 2016  // 288/day x 7 days

// Timing
#define HISTORY_WINDOW_US         (5ULL * 60ULL * 1000000ULL)  // 5 minutes (2016 entries = 7 days)
// #define HISTORY_WINDOW_US         (10ULL * 1000000ULL)  // 10 seconds (DEBUG)
// #define HISTORY_WINDOW_US         (1ULL * 1000000ULL)  // 1 second (DEBUG - fast overflow test)

// Sequence number for erased/empty slots
#define HISTORY_SEQ_EMPTY         0xFFFF

/**
 * @brief Single history slot stored in flash (32 bytes, packed)
 *
 * All temperature/humidity values are stored as int16 x100
 * (e.g., 25.43°C -> 2543, 65.21% -> 6521)
 * AQI/eCO2/eTVOC are stored as raw uint16 values.
 * sequence == 0xFFFF means the slot is erased/empty.
 */
typedef struct __attribute__((packed)) {
    uint16_t sequence;      // Monotonic counter, 0xFFFF = empty

    int16_t  temp_avg;      // Temperature average (x100 °C)
    int16_t  temp_min;      // Temperature minimum (x100 °C)
    int16_t  temp_max;      // Temperature maximum (x100 °C)

    int16_t  hum_avg;       // Humidity average (x100 %)
    int16_t  hum_min;       // Humidity minimum (x100 %)
    int16_t  hum_max;       // Humidity maximum (x100 %)

    uint16_t aqi_avg;       // AQI average
    uint16_t aqi_min;       // AQI minimum
    uint16_t aqi_max;       // AQI maximum

    uint16_t eco2_avg;      // eCO2 average (ppm)
    uint16_t eco2_min;      // eCO2 minimum (ppm)
    uint16_t eco2_max;      // eCO2 maximum (ppm)

    uint16_t etvoc_avg;     // eTVOC average (ppb)
    uint16_t etvoc_min;     // eTVOC minimum (ppb)
    uint16_t etvoc_max;     // eTVOC maximum (ppb)
} history_slot_t;

_Static_assert(sizeof(history_slot_t) == HISTORY_SLOT_SIZE,
               "history_slot_t must be exactly 32 bytes");

/**
 * @brief In-RAM accumulator for the current 10-minute window
 */
typedef struct {
    // Running sums for averaging
    float    sum_temp;
    float    sum_hum;
    uint32_t sum_aqi;
    uint32_t sum_eco2;
    uint32_t sum_etvoc;

    // Running min/max
    int16_t  min_temp;      // x100
    int16_t  max_temp;      // x100
    int16_t  min_hum;       // x100
    int16_t  max_hum;       // x100
    uint16_t min_aqi;
    uint16_t max_aqi;
    uint16_t min_eco2;
    uint16_t max_eco2;
    uint16_t min_etvoc;
    uint16_t max_etvoc;

    uint32_t sample_count;
    int64_t  window_start_us;  // esp_timer_get_time() at window start
} history_accumulator_t;

/**
 * @brief Initialize the history module
 *
 * Finds the flash partition, scans sequence numbers to determine
 * the current write position, and starts the accumulator.
 *
 * @return ESP_OK on success
 */
esp_err_t history_init(void);

/**
 * @brief Record a sensor sample into the accumulator
 *
 * Call this every time new sensor data is available (~1 second).
 * The sample is accumulated in RAM (sum/min/max).
 *
 * @param temp_c   Temperature in Celsius
 * @param humidity Relative humidity in percent
 * @param aqi      Air Quality Index
 * @param eco2     Equivalent CO2 in ppm
 * @param etvoc    Equivalent TVOC in ppb
 */
void history_record_sample(float temp_c, float humidity, int aqi, int eco2, int etvoc);

/**
 * @brief Check if the 10-minute window has elapsed and flush if so
 *
 * If the window has elapsed, computes avg/min/max from the accumulator,
 * writes a slot to flash (erasing the target sector if needed), and
 * resets the accumulator for the next window.
 *
 * @return true if a slot was flushed, false otherwise
 */
bool history_check_flush(void);

/**
 * @brief Get history metadata
 *
 * @param[out] write_index  Current write index in the ring buffer
 * @param[out] entry_count  Number of valid entries (max HISTORY_MAX_VALID_ENTRIES)
 */
void history_get_info(uint16_t *write_index, uint16_t *entry_count);

/**
 * @brief Read a single history slot by logical index
 *
 * Logical index 0 = oldest valid entry, entry_count-1 = newest.
 *
 * @param logical_index  Logical index (0 = oldest)
 * @param[out] slot      Pointer to slot structure to fill
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t history_read_slot(uint16_t logical_index, history_slot_t *slot);

/**
 * @brief Clear all history data
 *
 * Erases the entire flash partition and resets the write position.
 *
 * @return ESP_OK on success
 */
esp_err_t history_clear(void);

#endif // HISTORY_H
