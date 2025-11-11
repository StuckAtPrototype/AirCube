//
// Created by glina126 on 10/9/2023.
//

#include "environmental.h"

// static functions
static void environmentalTaskImp(void *pvParameter);


static void environmentalTaskImp(void *pvParameter){
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

}

void environmentalTask(void *pvParameter){
    environmentalTaskImp(pvParameter);

}