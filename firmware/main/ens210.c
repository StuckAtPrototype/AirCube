
#include "ens210.h"
#include "i2c_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define ENS210_I2C_ADDRESS 0x43

#define ENS210_REG_PART_ID 0x00
#define ENS210_REG_SYS_CTRL 0x10
#define ENS210_REG_SYS_STAT 0x11
#define ENS210_REG_SENS_RUN 0x21
#define ENS210_REG_SENS_START 0x22
#define ENS210_REG_T_VAL 0x30
#define ENS210_REG_H_VAL 0x33


uint8_t ens210_t[2];
uint8_t ens210_h[2];

float temperature_K;
float temperature_C;
float temperature_F;
float humidity_percentage;

void ens210_get_envir(uint8_t * t, uint8_t * h){
    t[0] = ens210_t[0];
    t[1] = ens210_t[1];

    h[0] = ens210_h[0];
    h[1] = ens210_h[1];
}

void ens210_set_mode(void){

}

// 0 = F
// 1 = C
// 2 = K
float ens210_get_temperature(uint8_t type){
    float temperature = 0;

    switch (type) {
        case 0:
            temperature = temperature_F;
            break;
        case 1:
            temperature = temperature_C;
            break;
        case 2:
            temperature = temperature_K;
            break;
        default:
            temperature = temperature_F;
            break;
    }

    return temperature;
}

float ens210_get_humidity(void){
    return humidity_percentage;
}

uint8_t ens210_get_status(void){
    uint8_t i2c_data[1];
    uint8_t i2c_byte_address[1];
    
    i2c_byte_address[0] = ENS210_REG_SYS_STAT;
    i2c_data[0] = 0;
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 1);
    
    return i2c_data[0];
}

void ens210_deinit(void){
    uint8_t i2c_data[2];
    i2c_data[0] = ENS210_REG_SYS_CTRL;
    i2c_data[1] = 0x01;

    i2c_driver_write(ENS210_I2C_ADDRESS, i2c_data, 2);
}

void ens210_read_envir(void){
    // In continuous mode, sensor automatically updates T_VAL and H_VAL registers
    // T_VAL and H_VAL are 3-byte registers: [DATA_LSB, DATA_MSB, VALID+CRC]
    // Format: bits 15:0 = DATA (little endian), bit 16 = VALID, bits 23:17 = CRC
    uint8_t i2c_data[3];
    uint8_t i2c_byte_address[1];

    // Read temperature value (3-byte register at address 0x30)
    i2c_byte_address[0] = ENS210_REG_T_VAL;
    memset(i2c_data, 0, sizeof(i2c_data));
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 3);

    // Extract 24-bit value (little endian): [0]=LSB, [1]=MSB, [2]=VALID+CRC
    uint32_t t_val = ((uint32_t)i2c_data[0]) | 
                     ((uint32_t)i2c_data[1] << 8) | 
                     ((uint32_t)i2c_data[2] << 16);
    
    // Extract data (lower 16 bits) and validity bit (bit 16)
    uint32_t t_data = (t_val >> 0) & 0xFFFF;
    uint32_t t_valid = (t_val >> 16) & 0x1;

    if (t_valid) {
        ens210_t[0] = i2c_data[0];
        ens210_t[1] = i2c_data[1];
        
        float TinK = (float)t_data / 64.0f; // Temperature in Kelvin (1/64 K per LSB)
        float TinC = TinK - 273.15f; // Temperature in Celsius
        float TinF = TinC * 1.8f + 32.0f; // Temperature in Fahrenheit

        temperature_K = TinK;
        temperature_C = TinC;
        temperature_F = TinF;

        ESP_LOGI("ens210", "%5.1fK %4.1fC %4.1fF", TinK, TinC, TinF);
    } else {
        ESP_LOGW("ens210", "Temperature data not valid");
    }

    // Read humidity value (3-byte register at address 0x33)
    i2c_byte_address[0] = ENS210_REG_H_VAL;
    memset(i2c_data, 0, sizeof(i2c_data));
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 3);

    // Extract 24-bit value (little endian): [0]=LSB, [1]=MSB, [2]=VALID+CRC
    uint32_t h_val = ((uint32_t)i2c_data[0]) | 
                     ((uint32_t)i2c_data[1] << 8) | 
                     ((uint32_t)i2c_data[2] << 16);
    
    // Extract data (lower 16 bits) and validity bit (bit 16)
    uint32_t h_data = (h_val >> 0) & 0xFFFF;
    uint32_t h_valid = (h_val >> 16) & 0x1;

    if (h_valid) {
        ens210_h[0] = i2c_data[0];
        ens210_h[1] = i2c_data[1];
        
        float H = (float)h_data / 512.0f; // Relative humidity in % (1/512 %RH per LSB)
        humidity_percentage = H;
        ESP_LOGD("ens210", "Humidity: %2.0f%%", H);
    } else {
        ESP_LOGW("ens210", "Humidity data not valid");
    }
}



void ens210_init(void){
    // Configure ENS210 for continuous mode operation
    uint8_t i2c_data[2];
    uint8_t i2c_byte_address[1];

    // Set the system into active mode (disable low power mode)
    // SYS_CTRL bit 0: LOW_POWER (0=disabled, device stays active)
    i2c_data[0] = ENS210_REG_SYS_CTRL;
    i2c_data[1] = 0b0; // Disable low power mode (device stays in active state)
    i2c_driver_write(ENS210_I2C_ADDRESS, i2c_data, 2);

    // Enable continuous mode for both temperature and humidity
    // SENS_RUN: bit 0 = T_RUN (temperature), bit 1 = H_RUN (humidity)
    // 1 = continuous mode, 0 = single shot mode
    i2c_data[0] = ENS210_REG_SENS_RUN;
    i2c_data[1] = 0b11; // Enable both temperature and humidity in continuous mode
    i2c_driver_write(ENS210_I2C_ADDRESS, i2c_data, 2);

    // Start the first measurement in continuous mode
    // SENS_START: bit 0 = T_START, bit 1 = H_START
    // Write 1 to start measurement (writing 0 has no effect)
    i2c_data[0] = ENS210_REG_SENS_START;
    i2c_data[1] = 0b11; // Start both temperature and humidity measurement
    i2c_driver_write(ENS210_I2C_ADDRESS, i2c_data, 2);

    // Wait for first measurement to complete
    // T and RH continuous mode: 225-238ms typical
    vTaskDelay(250 / portTICK_PERIOD_MS);

    // Read system status to verify sensor is active
    i2c_byte_address[0] = ENS210_REG_SYS_STAT;
    i2c_data[0] = 0;
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 1);

    ESP_LOGI("ens210", "ENS210 initialized in continuous mode, SYS_STAT: 0x%02X", i2c_data[0]);
}