//
// Shared I2C driver for ENS210 and ENS16X sensors
//

#include "i2c_driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_MASTER_SCL_IO           0      /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           1      /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              0      /*!< I2C master i2c port number */
#define I2C_MASTER_FREQ_HZ          100000 /*!< I2C master clock frequency */
#define I2C_MASTER_TIMEOUT_MS       1000

static const char *TAG = "i2c_driver";
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static bool i2c_initialized = false;

// Cache for device handles (simple implementation - can be extended for more devices)
#define MAX_CACHED_DEVICES 4
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t handle;
    bool in_use;
} device_cache[MAX_CACHED_DEVICES] = {0};

static i2c_master_dev_handle_t get_device_handle(uint8_t device_addr)
{
    // Check if device is already cached
    for (int i = 0; i < MAX_CACHED_DEVICES; i++) {
        if (device_cache[i].in_use && device_cache[i].addr == device_addr) {
            return device_cache[i].handle;
        }
    }

    // Find empty slot and create new device handle
    for (int i = 0; i < MAX_CACHED_DEVICES; i++) {
        if (!device_cache[i].in_use) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = device_addr,
                .scl_speed_hz = I2C_MASTER_FREQ_HZ,
            };

            esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &device_cache[i].handle);
            if (ret == ESP_OK) {
                device_cache[i].addr = device_addr;
                device_cache[i].in_use = true;
                return device_cache[i].handle;
            } else {
                ESP_LOGE(TAG, "Failed to add I2C device 0x%02X: %s", device_addr, esp_err_to_name(ret));
                return NULL;
            }
        }
    }

    ESP_LOGE(TAG, "Device cache full, cannot add device 0x%02X", device_addr);
    return NULL;
}

esp_err_t i2c_driver_init(void)
{
    if (i2c_initialized) {
        ESP_LOGW(TAG, "I2C driver already initialized");
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_mst_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_initialized = true;
    ESP_LOGI(TAG, "I2C driver initialized successfully");
    return ESP_OK;
}

esp_err_t i2c_driver_write(uint8_t device_addr, const uint8_t *data, size_t len)
{
    if (!i2c_initialized || i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t dev_handle = get_device_handle(device_addr);
    if (dev_handle == NULL) {
        return ESP_FAIL;
    }

    // Transmit data (register address + data)
    return i2c_master_transmit(dev_handle, data, len, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

esp_err_t i2c_driver_read(uint8_t device_addr, const uint8_t *reg_addr, size_t reg_len, uint8_t *data, size_t data_len)
{
    if (!i2c_initialized || i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t dev_handle = get_device_handle(device_addr);
    if (dev_handle == NULL) {
        return ESP_FAIL;
    }

    // Transmit register address and receive data
    return i2c_master_transmit_receive(dev_handle, reg_addr, reg_len, data, data_len, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

void i2c_driver_deinit(void)
{
    // Remove all cached device handles
    for (int i = 0; i < MAX_CACHED_DEVICES; i++) {
        if (device_cache[i].in_use) {
            i2c_master_bus_rm_device(device_cache[i].handle);
            device_cache[i].in_use = false;
            device_cache[i].addr = 0;
            device_cache[i].handle = NULL;
        }
    }

    if (i2c_bus_handle != NULL) {
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
        i2c_initialized = false;
        ESP_LOGI(TAG, "I2C driver deinitialized");
    }
}

