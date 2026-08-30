#include "spi_bus.h"
#include "esp_check.h"

static const char *TAG = "spi_bus";

static SemaphoreHandle_t s_spi_mutex = NULL;
static bool s_bus_initialized = false;

esp_err_t spi_bus_shared_init(void)
{
    if (s_bus_initialized) {
        return ESP_OK; // already done, avoid double init
    }

    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = SPI_BUS_SCK_GPIO,
        .mosi_io_num = SPI_BUS_MOSI_GPIO,
        .miso_io_num = SPI_BUS_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // Big enough for a full 240x320 RGB565 frame; shrink if RAM is tight
        // and you flush in smaller chunks instead.
        .max_transfer_sz = 240 * 320 * 2,
    };

    esp_err_t err = spi_bus_initialize(SHARED_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_spi_mutex);
        s_spi_mutex = NULL;
        return err;
    }

    s_bus_initialized = true;
    return ESP_OK;
}

void spi_bus_lock(void)
{
    // configASSERT would fire here in debug builds if init was skipped
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
}

void spi_bus_unlock(void)
{
    xSemaphoreGive(s_spi_mutex);
}
