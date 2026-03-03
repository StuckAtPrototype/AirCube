//
// History Module Implementation
// Append-only ring buffer for 14-day sensor history on flash partition
//

#include "history.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <limits.h>

static const char *TAG = "history";

// Partition label in the partition table
#define HISTORY_PARTITION_LABEL "history"
#define HISTORY_PARTITION_TYPE  ESP_PARTITION_TYPE_DATA
#define HISTORY_PARTITION_SUBTYPE 0x99

// Module state
static const esp_partition_t *s_partition = NULL;
static uint16_t s_write_index = 0;       // Next slot to write (0 .. HISTORY_TOTAL_SLOTS-1)
static uint16_t s_next_sequence = 0;     // Next sequence number to assign
static uint16_t s_valid_count = 0;       // Number of valid (non-empty) slots
static history_accumulator_t s_accum;    // Current window accumulator
static bool s_initialized = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Compare two uint16 sequence numbers with wraparound
 *
 * Returns true if seq_a is "newer" than seq_b, handling uint16 wraparound.
 * Uses the convention that if the unsigned difference is > 32768,
 * the smaller value has wrapped and is actually newer.
 */
static bool seq_is_newer(uint16_t seq_a, uint16_t seq_b)
{
    // Signed comparison of the difference handles wraparound
    return (int16_t)(seq_a - seq_b) > 0;
}

/**
 * @brief Calculate the flash offset for a given slot index
 */
static inline size_t slot_offset(uint16_t index)
{
    return (size_t)index * HISTORY_SLOT_SIZE;
}

/**
 * @brief Calculate which sector a slot belongs to
 */
static inline uint16_t slot_sector(uint16_t index)
{
    return index / HISTORY_SLOTS_PER_SECTOR;
}

/**
 * @brief Check if a slot is at the start of a sector
 */
static inline bool slot_is_sector_start(uint16_t index)
{
    return (index % HISTORY_SLOTS_PER_SECTOR) == 0;
}

/**
 * @brief Read just the sequence number from a slot in flash
 */
static esp_err_t read_slot_sequence(uint16_t index, uint16_t *seq_out)
{
    return esp_partition_read(s_partition, slot_offset(index), seq_out, sizeof(uint16_t));
}

/**
 * @brief Erase the sector containing the given slot index
 */
static esp_err_t erase_slot_sector(uint16_t index)
{
    uint16_t sector = slot_sector(index);
    size_t offset = (size_t)sector * HISTORY_SECTOR_SIZE;

    ESP_LOGI(TAG, "Erasing sector %u (offset 0x%x)", sector, (unsigned)offset);
    return esp_partition_erase_range(s_partition, offset, HISTORY_SECTOR_SIZE);
}

/**
 * @brief Scan all slots to find write position and entry count
 *
 * Reads the sequence number from every slot. Finds the slot with the
 * highest sequence number (handling uint16 wraparound) to determine
 * where to write next.
 */
static esp_err_t scan_slots(void)
{
    uint16_t highest_seq = 0;
    int32_t highest_index = -1;  // -1 means no valid slots found
    uint16_t valid_count = 0;

    for (uint16_t i = 0; i < HISTORY_TOTAL_SLOTS; i++) {
        uint16_t seq;
        esp_err_t err = read_slot_sequence(i, &seq);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read slot %u sequence: %s", i, esp_err_to_name(err));
            return err;
        }

        if (seq == HISTORY_SEQ_EMPTY) {
            continue;  // Erased slot, skip
        }

        valid_count++;

        if (highest_index < 0 || seq_is_newer(seq, highest_seq)) {
            highest_seq = seq;
            highest_index = i;
        }
    }

    if (highest_index < 0) {
        // No valid slots found -- fresh/cleared partition
        s_write_index = 0;
        s_next_sequence = 1;  // Start at 1 (0xFFFF is reserved for empty)
        s_valid_count = 0;
        ESP_LOGI(TAG, "Partition is empty, starting fresh");
    } else {
        s_write_index = (highest_index + 1) % HISTORY_TOTAL_SLOTS;
        s_next_sequence = highest_seq + 1;
        if (s_next_sequence == HISTORY_SEQ_EMPTY) {
            s_next_sequence = 0;  // Skip 0xFFFF on wraparound
        }
        s_valid_count = valid_count;
        ESP_LOGI(TAG, "Found %u valid entries, newest at slot %ld (seq %u), next write at %u",
                 valid_count, (long)highest_index, highest_seq, s_write_index);
    }

    return ESP_OK;
}

/**
 * @brief Reset the accumulator for a new window
 */
static void reset_accumulator(void)
{
    memset(&s_accum, 0, sizeof(s_accum));
    s_accum.min_temp = INT16_MAX;
    s_accum.max_temp = INT16_MIN;
    s_accum.min_hum = INT16_MAX;
    s_accum.max_hum = INT16_MIN;
    s_accum.min_aqi = UINT16_MAX;
    s_accum.max_aqi = 0;
    s_accum.min_eco2 = UINT16_MAX;
    s_accum.max_eco2 = 0;
    s_accum.min_etvoc = UINT16_MAX;
    s_accum.max_etvoc = 0;
    s_accum.sample_count = 0;
    s_accum.window_start_us = esp_timer_get_time();
}

/**
 * @brief Convert float to int16 x100 with clamping
 */
static int16_t float_to_x100(float val)
{
    float scaled = val * 100.0f;
    if (scaled > (float)INT16_MAX) return INT16_MAX;
    if (scaled < (float)INT16_MIN) return INT16_MIN;
    return (int16_t)(scaled + (scaled >= 0 ? 0.5f : -0.5f));
}

/**
 * @brief Clamp an integer to uint16 range
 */
static uint16_t clamp_u16(int val)
{
    if (val < 0) return 0;
    if (val > UINT16_MAX) return UINT16_MAX;
    return (uint16_t)val;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t history_init(void)
{
    // Find the partition
    s_partition = esp_partition_find_first(
        HISTORY_PARTITION_TYPE, HISTORY_PARTITION_SUBTYPE, HISTORY_PARTITION_LABEL);

    if (s_partition == NULL) {
        ESP_LOGE(TAG, "History partition '%s' not found!", HISTORY_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found partition '%s' at offset 0x%lx, size %lu bytes",
             s_partition->label, (unsigned long)s_partition->address,
             (unsigned long)s_partition->size);

    // Verify partition is large enough
    if (s_partition->size < (size_t)HISTORY_TOTAL_SLOTS * HISTORY_SLOT_SIZE) {
        ESP_LOGE(TAG, "Partition too small: need %u bytes, have %lu",
                 HISTORY_TOTAL_SLOTS * HISTORY_SLOT_SIZE, (unsigned long)s_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Scan existing data to find write position
    esp_err_t err = scan_slots();
    if (err != ESP_OK) {
        return err;
    }

    // Start the accumulator
    reset_accumulator();
    s_initialized = true;

    ESP_LOGI(TAG, "History initialized: %u valid entries, write position %u",
             s_valid_count, s_write_index);
    return ESP_OK;
}

void history_record_sample(float temp_c, float humidity, int aqi, int eco2, int etvoc)
{
    if (!s_initialized) return;

    int16_t temp_x100 = float_to_x100(temp_c);
    int16_t hum_x100 = float_to_x100(humidity);
    uint16_t aqi_u16 = clamp_u16(aqi);
    uint16_t eco2_u16 = clamp_u16(eco2);
    uint16_t etvoc_u16 = clamp_u16(etvoc);

    // Accumulate sums
    s_accum.sum_temp += temp_c;
    s_accum.sum_hum += humidity;
    s_accum.sum_aqi += aqi_u16;
    s_accum.sum_eco2 += eco2_u16;
    s_accum.sum_etvoc += etvoc_u16;

    // Update min/max
    if (temp_x100 < s_accum.min_temp) s_accum.min_temp = temp_x100;
    if (temp_x100 > s_accum.max_temp) s_accum.max_temp = temp_x100;
    if (hum_x100 < s_accum.min_hum) s_accum.min_hum = hum_x100;
    if (hum_x100 > s_accum.max_hum) s_accum.max_hum = hum_x100;
    if (aqi_u16 < s_accum.min_aqi) s_accum.min_aqi = aqi_u16;
    if (aqi_u16 > s_accum.max_aqi) s_accum.max_aqi = aqi_u16;
    if (eco2_u16 < s_accum.min_eco2) s_accum.min_eco2 = eco2_u16;
    if (eco2_u16 > s_accum.max_eco2) s_accum.max_eco2 = eco2_u16;
    if (etvoc_u16 < s_accum.min_etvoc) s_accum.min_etvoc = etvoc_u16;
    if (etvoc_u16 > s_accum.max_etvoc) s_accum.max_etvoc = etvoc_u16;

    s_accum.sample_count++;
}

bool history_check_flush(void)
{
    if (!s_initialized) return false;
    if (s_accum.sample_count == 0) return false;

    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_accum.window_start_us;

    if (elapsed < (int64_t)HISTORY_WINDOW_US) {
        return false;  // Window not yet elapsed
    }

    // Compute averages
    float n = (float)s_accum.sample_count;
    history_slot_t slot;
    slot.sequence = s_next_sequence;

    slot.temp_avg = float_to_x100(s_accum.sum_temp / n);
    slot.temp_min = s_accum.min_temp;
    slot.temp_max = s_accum.max_temp;

    slot.hum_avg = float_to_x100(s_accum.sum_hum / n);
    slot.hum_min = s_accum.min_hum;
    slot.hum_max = s_accum.max_hum;

    slot.aqi_avg = (uint16_t)(s_accum.sum_aqi / s_accum.sample_count);
    slot.aqi_min = s_accum.min_aqi;
    slot.aqi_max = s_accum.max_aqi;

    slot.eco2_avg = (uint16_t)(s_accum.sum_eco2 / s_accum.sample_count);
    slot.eco2_min = s_accum.min_eco2;
    slot.eco2_max = s_accum.max_eco2;

    slot.etvoc_avg = (uint16_t)(s_accum.sum_etvoc / s_accum.sample_count);
    slot.etvoc_min = s_accum.min_etvoc;
    slot.etvoc_max = s_accum.max_etvoc;

    // Check if the target slot's sector needs erasing
    // If the slot is at the start of a sector, or if the slot is not empty,
    // we need to erase the whole sector first.
    uint16_t target_seq;
    esp_err_t err = read_slot_sequence(s_write_index, &target_seq);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read target slot %u: %s", s_write_index, esp_err_to_name(err));
        reset_accumulator();
        return false;
    }

    if (target_seq != HISTORY_SEQ_EMPTY) {
        // Slot is occupied by old data -- erase the entire sector
        ESP_LOGW(TAG, ">>> OVERWRITING old data! Slot %u has seq %u, erasing sector <<<",
                 s_write_index, target_seq);
        err = erase_slot_sector(s_write_index);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase sector for slot %u: %s",
                     s_write_index, esp_err_to_name(err));
            reset_accumulator();
            return false;
        }

        // Recalculate valid count: subtract the slots we just erased in this sector
        uint16_t sector_start = slot_sector(s_write_index) * HISTORY_SLOTS_PER_SECTOR;
        uint16_t sector_end = sector_start + HISTORY_SLOTS_PER_SECTOR;
        if (sector_end > HISTORY_TOTAL_SLOTS) {
            sector_end = HISTORY_TOTAL_SLOTS;
        }
        // All slots in this sector are now erased, subtract them from valid count
        // (the slot we just erased was counted, minus 1 because we're about to write one)
        uint16_t erased_in_sector = sector_end - sector_start;
        if (s_valid_count >= erased_in_sector) {
            s_valid_count -= erased_in_sector;
        } else {
            s_valid_count = 0;
        }
    }

    // Write the slot
    err = esp_partition_write(s_partition, slot_offset(s_write_index), &slot, sizeof(slot));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write slot %u: %s", s_write_index, esp_err_to_name(err));
        reset_accumulator();
        return false;
    }

    ESP_LOGI(TAG, "Flushed slot %u (seq %u, %lu samples, temp_avg=%d, aqi_avg=%u)",
             s_write_index, slot.sequence, (unsigned long)s_accum.sample_count,
             slot.temp_avg, slot.aqi_avg);

    // Advance state
    s_valid_count++;
    uint16_t old_write_index = s_write_index;
    s_write_index = (s_write_index + 1) % HISTORY_TOTAL_SLOTS;
    s_next_sequence++;
    if (s_next_sequence == HISTORY_SEQ_EMPTY) {
        s_next_sequence = 0;  // Skip 0xFFFF
    }

    // *** DEBUG: Ring buffer wrap-around detection ***
    if (s_write_index == 0) {
        ESP_LOGW(TAG, ">>> RING BUFFER WRAPPED! Write index rolled from %u back to 0. Total valid: %u <<<",
                 old_write_index, s_valid_count);
    }

    // Reset accumulator for next window
    reset_accumulator();
    return true;
}

void history_get_info(uint16_t *write_index, uint16_t *entry_count)
{
    if (write_index) *write_index = s_write_index;

    uint16_t count = s_valid_count;
    if (count > HISTORY_MAX_VALID_ENTRIES) {
        count = HISTORY_MAX_VALID_ENTRIES;
    }
    if (entry_count) *entry_count = count;
}

esp_err_t history_read_slot(uint16_t logical_index, history_slot_t *slot)
{
    if (!s_initialized || slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t count = s_valid_count;
    if (count > HISTORY_MAX_VALID_ENTRIES) {
        count = HISTORY_MAX_VALID_ENTRIES;
    }

    if (logical_index >= count) {
        return ESP_ERR_INVALID_ARG;
    }

    // Logical index 0 = oldest valid entry
    // The newest entry is at (s_write_index - 1 + TOTAL) % TOTAL
    // The oldest of `count` entries is at (s_write_index - count + TOTAL) % TOTAL
    // So logical_index maps to: (s_write_index - count + logical_index + TOTAL) % TOTAL
    uint16_t physical_index = (s_write_index - count + logical_index + HISTORY_TOTAL_SLOTS)
                              % HISTORY_TOTAL_SLOTS;

    esp_err_t err = esp_partition_read(s_partition, slot_offset(physical_index),
                                       slot, sizeof(history_slot_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read slot at physical index %u: %s",
                 physical_index, esp_err_to_name(err));
        return err;
    }

    // Verify the slot is valid (not erased)
    if (slot->sequence == HISTORY_SEQ_EMPTY) {
        // This can happen briefly after a sector erase -- skip it
        memset(slot, 0, sizeof(history_slot_t));
        slot->sequence = HISTORY_SEQ_EMPTY;
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

esp_err_t history_clear(void)
{
    if (!s_initialized || s_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Clearing all history data");

    // Erase the entire partition
    esp_err_t err = esp_partition_erase_range(s_partition, 0, s_partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase partition: %s", esp_err_to_name(err));
        return err;
    }

    // Reset state
    s_write_index = 0;
    s_next_sequence = 1;
    s_valid_count = 0;
    reset_accumulator();

    ESP_LOGI(TAG, "History cleared");
    return ESP_OK;
}
