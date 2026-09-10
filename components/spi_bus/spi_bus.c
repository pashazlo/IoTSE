#include "spi_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "SPI_BUS";

// Максимальный размер одной SPI-транзакции — под самый большой кадр
// дисплея (320x170, RGB565 = 2 байта/пиксель). Задаём числом явно,
// А НЕ через DISPLAY_WIDTH/DISPLAY_HEIGHT из display.h — spi_bus стоит
// НИЖЕ display в иерархии зависимостей (display.h подключает spi_bus.h,
// а не наоборот), подключение display.h сюда создало бы обратную
// зависимость и не скомпилировалось бы вообще.
#define SPI_BUS_MAX_TRANSFER_SZ   (320 * 170 * 2)

static SemaphoreHandle_t s_spi_mutex = NULL;
static bool s_bus_initialized = false;


// ============================================================================
// Public Functions
// ============================================================================

esp_err_t spi_bus_shared_init(void)
{
    if (s_bus_initialized) {
        ESP_LOGI(TAG, "SPI bus already initialized, skipping");
        return ESP_OK;
    }

    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI bus mutex (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = SPI_BUS_SCK_GPIO,
        .mosi_io_num = SPI_BUS_MOSI_GPIO,
        .miso_io_num = SPI_BUS_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUS_MAX_TRANSFER_SZ,
    };

    esp_err_t err = spi_bus_initialize(SHARED_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_spi_mutex);
        s_spi_mutex = NULL;
        return err;
    }

    s_bus_initialized = true;

    ESP_LOGI(TAG, "SPI bus initialized on host %d (SCK=%d, MOSI=%d, MISO=%d)",
             SHARED_SPI_HOST, SPI_BUS_SCK_GPIO, SPI_BUS_MOSI_GPIO, SPI_BUS_MISO_GPIO);

    return ESP_OK;
}


bool spi_bus_lock(TickType_t timeout)
{
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "lock: SPI bus is not initialized — call spi_bus_shared_init() first");
        return false;
    }

    if (xSemaphoreTake(s_spi_mutex, timeout) != pdTRUE) {
        // Сюда попадаем либо если шина реально занята дольше timeout
        // (кто-то держит лок и не отпускает — ищите непарный
        // lock()/unlock() в коде-потребителе), либо timeout слишком
        // короткий для текущей операции. В любом случае — теперь это
        // ВИДНО в логе, а не тихое зависание навсегда.
        ESP_LOGE(TAG, "SPI lock timeout (%lu ticks) — bus busy or deadlocked",
                 (unsigned long)timeout);
        return false;
    }

    return true;
}


void spi_bus_unlock(void)
{
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "unlock: SPI bus is not initialized");
        return;
    }

    if (xSemaphoreGive(s_spi_mutex) != pdTRUE) {
        // Обычно означает unlock() без предшествующего успешного lock() —
        // симптом бага в вызывающем коде, а не в самой шине.
        ESP_LOGE(TAG, "SPI mutex give failed — unmatched unlock()?");
    }
}
