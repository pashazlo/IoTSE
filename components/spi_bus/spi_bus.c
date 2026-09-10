#include "spi_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "SPI_BUS";

static SemaphoreHandle_t s_spi_mutex = NULL;
static bool s_bus_initialized = false;


esp_err_t spi_bus_shared_init(void)
{
    if (s_bus_initialized) {
        return ESP_OK;
    }

    s_spi_mutex = xSemaphoreCreateMutex();

    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = SPI_BUS_SCK_GPIO,
        .mosi_io_num = SPI_BUS_MOSI_GPIO,
        .miso_io_num = SPI_BUS_MISO_GPIO,

        .quadwp_io_num = -1,
        .quadhd_io_num = -1,

        /*
         * ST7789 is 320x170.
         *
         * We do not need to advertise the old 320x240 frame here.
         */
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
    };

    esp_err_t err = spi_bus_initialize(
        SHARED_SPI_HOST,
        &buscfg,
        SPI_DMA_CH_AUTO
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "spi_bus_initialize failed: %s",
            esp_err_to_name(err)
        );

        vSemaphoreDelete(s_spi_mutex);
        s_spi_mutex = NULL;

        return err;
    }

    s_bus_initialized = true;

    ESP_LOGI(
        TAG,
        "SPI bus initialized: host=%d SCK=%d MOSI=%d MISO=%d",
        SHARED_SPI_HOST,
        SPI_BUS_SCK_GPIO,
        SPI_BUS_MOSI_GPIO,
        SPI_BUS_MISO_GPIO
    );

    return ESP_OK;
}


bool spi_bus_try_lock(TickType_t timeout)
{
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "try_lock: SPI bus is not initialized");
        return false;
    }

    BaseType_t result = xSemaphoreTake(
        s_spi_mutex,
        timeout
    );

    if (result != pdTRUE) {
        ESP_LOGE(
            TAG,
            "SPI mutex timeout! owner may be stuck"
        );

        return false;
    }

    return true;
}


bool spi_bus_try_lock(TickType_t timeout)
{
    if (!spi_bus_try_lock(pdMS_TO_TICKS(1000))) {
        /*
         * Do NOT wait forever.
         *
         * This is deliberately fatal to the current operation rather than
         * silently freezing the entire firmware.
         */
        ESP_LOGE(
            TAG,
            "SPI lock failed after 1000 ms"
        );
    }
}


bool spi_bus_lock(TickType_t timeout)
{
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "unlock: SPI bus is not initialized");
        return;
    }

    BaseType_t result = xSemaphoreGive(s_spi_mutex);

    if (result != pdTRUE) {
        ESP_LOGE(TAG, "SPI mutex give failed");
    }
}
