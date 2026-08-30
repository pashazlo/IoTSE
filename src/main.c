#include "spi_bus.h"
#include "display.h"

void app_main(void)
{
    ESP_ERROR_CHECK(spi_bus_shared_init());
    ESP_ERROR_CHECK(display_init());

    display_fill_color(0xF800);  // красный экран для проверки

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
