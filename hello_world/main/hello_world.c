#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_system.h"

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("Hello world from %s with %d CPU core(s)!\n",
           CONFIG_IDF_TARGET, chip_info.cores);

    int count = 0;
    while (1) {
        printf("Hello world! (%d)\n", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
