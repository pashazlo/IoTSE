#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spiram.h"

void app_main(void)
{
    printf("\n\n=== RF Tool starting ===\n");
    printf("CPU: %s\n", CONFIG_IDF_TARGET);
    printf("Free heap: %lu bytes\n", esp_get_free_heap_size());
    printf("PSRAM size: %d bytes\n", esp_spiram_get_size());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        printf("Tick... free heap: %lu\n", esp_get_free_heap_size());
    }
}
