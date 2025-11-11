//
// Created by glina126 on 11/8/2023.
//

#include "ens16x_driver.h"
#include "i2c_driver.h"
#include <string.h>
#include "esp_log.h"

#define ENS16X_I2C_ADDRESS 0x52

// registers
#define ENS16X_DEVICE_STATUS 0x20
#define ENS16X_OPMODE 0x10
#define ENS16X_REG_DATA_ETVOC 0x22
#define ENS16X_REG_DATA_ECO2 0x24
#define ENS16X_REG_DATA_AQI_S 0x26

#define ENS16X_REG_RH_IN 0x15
#define ENS16X_REG_TH_IN 0x13

enum ENS_OPMODE ens16x_op_mode = ENS_RESERVED;
enum ENS_STATUS ens16x_status = ENS_RESET;
uint8_t ens16x_new_data_available = 0;
uint8_t ens16x_new_gpr_available = 0;
int ens16x_tvoc = -1;
int ens16x_eco2 = -1;
int ens16x_aqi = -1;


enum ENS_STATUS ens16x_get_device_status(void);
enum ENS_OPMODE ens16x_get_opmode(void);
void ens16x_set_opmode(enum ENS_OPMODE mode);

enum ENS_STATUS ens16x_get_device_status(void){

    // read in ENS16x part number
    uint8_t i2c_data[1];
    memset(i2c_data, 0, 1);
    uint8_t i2c_byte_address[] = {ENS16X_DEVICE_STATUS};
    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 1);

    // bit 0 = NEWGPR
    // bit 1 = NEWDAT
    // bit 2-3 = VALIDITY FLAG
    // bit 6 = STATER - error invalid operating mode
    // bit 7 = STATS - OPMODE is running
    ens16x_new_data_available = (i2c_data[0] & (1U << 1)) >> 1;
    ens16x_new_gpr_available = (i2c_data[0] & (1U << 0)) >> 0;
    ens16x_status = (i2c_data[0] & (1U << 2)) >> 2 | (i2c_data[0] & (1U << 3)) >> 3;

    return i2c_data[0];

}

enum ENS_OPMODE ens16x_get_opmode(void){
    uint8_t i2c_data[1];
    memset(i2c_data, 0, 1);
    uint8_t i2c_byte_address[] = {ENS16X_OPMODE};
    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 1);
    return i2c_data[0];
}

void ens16x_set_opmode(enum ENS_OPMODE mode){
    uint8_t i2c_data[2];
    memset(i2c_data, 0, 2);
    i2c_data[0] = ENS16X_OPMODE;

    // transition the sensor to idle mode first
    i2c_data[1] = ENS_IDLE;
    i2c_driver_write(ENS16X_I2C_ADDRESS, i2c_data, 2);

    // transition to ENS_OPMODE
    i2c_data[1] = mode;
    i2c_driver_write(ENS16X_I2C_ADDRESS, i2c_data, 2);

    // confirm by reading
    uint8_t i2c_return_data[1];
    uint8_t reg_addr[1] = {ENS16X_OPMODE};
    i2c_driver_read(ENS16X_I2C_ADDRESS, reg_addr, 1, i2c_return_data, 1);
    ESP_LOGD("ens16x", "operational mode: %u", i2c_data[0]);
    if(i2c_return_data[0] == mode){
        ESP_LOGI("ens16x", "Mode change success");
    } else
        ESP_LOGE("ens16x", "Error changing modes");
}

int ens16x_read_etvoc(void){
    uint8_t i2c_data[2];
    memset(i2c_data, 0, 2);
    uint8_t i2c_byte_address[] = {ENS16X_REG_DATA_ETVOC};
    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 2);
    uint16_t etvoc = ((uint16_t )i2c_data[0] | (uint16_t)i2c_data[1] << 8);

    ESP_LOGD("ens16x", "ETVOC_LSB: %u, ETVOC_MSB: %u", i2c_data[0], i2c_data[1]);
    ESP_LOGI("ens16x", "etvoc: %hu", etvoc);
    ens16x_tvoc = etvoc;

    return etvoc;
}

int ens16x_read_eco2(void){
    uint8_t i2c_data[2];
    memset(i2c_data, 0, 2);
    uint8_t i2c_byte_address[] = {ENS16X_REG_DATA_ECO2};
    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 2);
    uint16_t eco2 = ((uint16_t )i2c_data[0] | (uint16_t)i2c_data[1] << 8);

    ESP_LOGD("ens16x", "ECO2_LSB: %u, ECO2_MSB: %u", i2c_data[0], i2c_data[1]);
    ESP_LOGI("ens16x", "eco2: %hu", eco2);
    ens16x_eco2 = eco2;

    return eco2;
}

int ens16x_read_aqi(void){
    uint8_t i2c_data[2];
    memset(i2c_data, 0, 2);
    uint8_t i2c_byte_address[] = {ENS16X_REG_DATA_AQI_S};
    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 2);
    uint16_t aqi = ((uint16_t )i2c_data[0] | (uint16_t)i2c_data[1] << 8);

    ESP_LOGI("ens16x", "aqi: %hu", aqi);
    ens16x_aqi = aqi;

    return aqi;
}

int ens16x_get_aqi(void){
    return ens16x_aqi;
}

int ens16x_get_etvoc(void){
    return ens16x_tvoc;
}

enum ENS_STATUS ens16x_get_status(void){
    return ens16x_status;
}

void ens16x_write_ens210_data(uint8_t * t, uint8_t * h){
    // write envir compensation data to ens16x
    uint8_t i2c_data[5] = {};
    i2c_data[0] = ENS16X_REG_TH_IN;
    i2c_data[1] = t[0];
    i2c_data[2] = t[1];
    i2c_data[3] = h[0];
    i2c_data[4] = h[1];

    // write the regs
    i2c_driver_write(ENS16X_I2C_ADDRESS, i2c_data, 5);

    // read back the temperature and humidity for verification
    uint8_t address[1] = {0x30};
    i2c_data[0] = 0;
    i2c_data[1] = 0;

    i2c_driver_read(ENS16X_I2C_ADDRESS, address, 1, i2c_data, 2);

    for(int i = 0; i < 2; i++){
        ESP_LOGD("ens16x", "temperature i2c_data[%i]: %x", i, i2c_data[i]);
    }
    uint32_t temperature = ((uint32_t)i2c_data[0] | ((uint32_t)i2c_data[1]) << 8) & 0xffff;

    float TinK = (float)temperature / 64; // Temperature in Kelvin
    float TinC = TinK - 273.15; // Temperature in Celsius
    float TinF = TinC * 1.8 + 32.0; // Temperature in Fahrenheit

    ESP_LOGD("ens16x", "%5.1fK %4.1fC %4.1fF", TinK, TinC, TinF);

    // check humidity
    address[0] = 0x32;
    i2c_data[0] = 0;
    i2c_data[1] = 0;
    i2c_driver_read(ENS16X_I2C_ADDRESS, address, 1, i2c_data, 2);

    for(int i = 0; i < 2; i++){
        ESP_LOGD("ens16x", "humidity i2c_data[%i]: %x", i, i2c_data[i]);
    }

    uint32_t humidity = ((uint32_t)i2c_data[0] | ((uint32_t)i2c_data[1]) << 8) & 0xffff;
    float H = (float)humidity/512;
    ESP_LOGD("ens16x", "%2.0f%%", H);


}

// initialize the sensor
void ens16x_init(void){

    // read in ENS16x part number
    uint8_t i2c_data[2];
    i2c_data[0] = 0;
    i2c_data[1] = 0;
    uint8_t i2c_byte_address[] = {0};

    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 2);

    for(int i = 0; i < 2; i++){
        ESP_LOGD("ens160", "i2c_data[%i]: %x", i, i2c_data[i]);
    }

    // gets and sets the local global variables
    ens16x_get_device_status();

    if(ens16x_status == ENS_NO_VALID_OUTPUT){
        ESP_LOGI("ens16x", "no valid output available from ENS16x");
    }else if (ens16x_status == ENS_WARM_UP){
        ESP_LOGI("ens16x", "warming up");
    }else if (ens16x_status == ENS_OP_OK){
        ESP_LOGI("ens16x", "ready");
    } else
        ESP_LOGI("ens16x", "operational status: %x", ens16x_status);

    // print out the ens16x_new_data_available and gpr status
    ESP_LOGI("ens16x", "ens16x_new_data_available: %x", ens16x_new_data_available);
    ESP_LOGI("ens16x", "ens16x_new_gpr_available: %x", ens16x_new_gpr_available);


    // Set operating mode (default to STANDARD mode)
    enum ENS_OPMODE default_mode = ENS_STANDARD;
    enum ENS_OPMODE op_mode = ens16x_get_opmode();
    if(op_mode != default_mode){
        ESP_LOGI("ens16x", "setting mode to STANDARD");
        ens16x_set_opmode(default_mode);
    } else {
        ESP_LOGI("ens16x", "operating mode already set to STANDARD");
    }

    ens16x_read_etvoc();
    ens16x_read_aqi();

    // read in the     // read in the AQI UBA index (1 to 5) (1 to 5)
    i2c_data[0] = 0;
    i2c_data[1] = 0;
    i2c_byte_address[0] = 0x21;

    i2c_driver_read(ENS16X_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 1);

    for(int i = 0; i < 1; i++){
        ESP_LOGI("ens160", "AQI index (1 to 5) %x", i2c_data[i]);
    }

}