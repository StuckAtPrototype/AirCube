//
// Created by glina126 on 10/9/2023.
//

#ifndef DN680R_ENVIRONMENTAL_H
#define DN680R_ENVIRONMENTAL_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// holds all information to be displayed
struct envir_struct{
    uint32_t voc;
    uint32_t temperature;
    uint32_t humidity;
    uint32_t pressure;
};

void environmentalTask(void *pvParameter);

// unit conversion methods

// convert VOC to 2.5pm

// convert humidity

// convert pressure

// convert temperature

#endif //DN680R_ENVIRONMENTAL_H
