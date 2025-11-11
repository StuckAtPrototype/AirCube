
#include "ens210.h"
#include "i2c_driver.h"
#include "esp_log.h"

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

    uint8_t i2c_data[2];
    uint8_t i2c_byte_address[1];

    // todo ensure this function waits for the sensor to be ready before reading

    // read temperature
    i2c_byte_address[0] = ENS210_REG_T_VAL;
    i2c_data[0] = 0;
    i2c_data[1] = 0;
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 2);

    for(int i = 0; i < 2; i++){
        ESP_LOGD("ens210", "temperature i2c_data[%i]: %x", i, i2c_data[i]);
    }
    ens210_t[0] = i2c_data[0];
    ens210_t[1] = i2c_data[1];
    uint32_t temperature = ((uint32_t)i2c_data[0] | ((uint32_t)i2c_data[1]) << 8) & 0xffff;
//    uint32_t t_data = (t_val>>0 ) & 0xffff;
//    uint32_t t_valid= (t_val>>16) & 0x1;

    float TinK = (float)temperature / 64; // Temperature in Kelvin
    float TinC = TinK - 273.15; // Temperature in Celsius
    float TinF = TinC * 1.8 + 32.0; // Temperature in Fahrenheit

    temperature_K = TinK;
    temperature_C = TinC;
    temperature_F = TinF;

    ESP_LOGD("ens210", "%5.1fK %4.1fC %4.1fF", TinK, TinC, TinF);

    // read humidity
    i2c_byte_address[0] = ENS210_REG_H_VAL;
    i2c_data[0] = 0;
    i2c_data[1] = 0;
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 2);

    for(int i = 0; i < 2; i++){
        ESP_LOGD("ens210", "humidity i2c_data[%i]: %x", i, i2c_data[i]);
    }

    ens210_h[0] = i2c_data[0];
    ens210_h[1] = i2c_data[1];

    uint32_t humidity = ((uint32_t)i2c_data[0] | ((uint32_t)i2c_data[1]) << 8) & 0xffff;
    // uint32_t h_valid= (h_val>>16) & 0x1
    float H = (float)humidity/512;
    ESP_LOGD("ens210", "%2.0f%%", H);

    humidity_percentage = H;
}



void ens210_init(void){
    // start a single shot conversion for temperature and humidity
    uint8_t i2c_data[2];
    uint8_t i2c_byte_address[1];


    /*
     *  Start a measurement (write 0b01, 0b10, or 011 to SENS_START)
        Wait for tbooting to get into active state (check SYS_ACTIVE to be 1)
        Read the ID register(s)
        Ensure the device is still in active state (check SYS_ACTIVE to be 1
     */

    // set the system into active mode
    i2c_data[0] = ENS210_REG_SYS_CTRL;
    i2c_data[1] = 0b0; // low power disable
    i2c_driver_write(ENS210_I2C_ADDRESS, i2c_data, 2);

    // initiate a single shot conversion
    i2c_data[0] = ENS210_REG_SENS_START;
    i2c_data[1] = 0b11; // both temperature and humidity
    i2c_driver_write(ENS210_I2C_ADDRESS, i2c_data, 2);


    // read system status
    i2c_byte_address[0] = ENS210_REG_SYS_STAT;
    i2c_data[0] = 0;
    i2c_data[1] = 0;
    i2c_driver_read(ENS210_I2C_ADDRESS, i2c_byte_address, 1, i2c_data, 1);

    for(int i = 0; i < 2; i++){
        ESP_LOGD("ens210", "sys stat i2c_data[%i]: %x", i, i2c_data[i]);
    }



}