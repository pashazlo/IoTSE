#include "spi_bus.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "spi_bus";

// ============================================================================
// Static State
// ============================================================================

// Mutex to synchronize access between multiple devices (display, SD card, etc.)
// When device A is using the bus, device B blocks until A releases it.
static SemaphoreHandle_t s_spi_mutex = NULL;

// Flag to prevent double-initialization
static bool s_bus_initialized = false;

// ============================================================================
// Public Functions
// ============================================================================

/**
 * Initialize the shared SPI bus and create the synchronization mutex.
 *
 * This must be called exactly once before any device driver init.
 * Attempting to initialize multiple times is safe (will return ESP_OK immediately).
 */
esp_err_t spi_bus_shared_init(void)
{
    // Guard against double initialization
    if (s_bus_initialized) {
        ESP_LOGI(TAG, "SPI bus already initialized, skipping");
        return ESP_OK;
    }

    // Create mutex for coordinating access across multiple bus devices.
    // This prevents, e.g., display DMA and SD card read from stepping on each other.
    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI bus mutex (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    // Configure the physical bus: SCK, MOSI, MISO lines and max transfer size
    spi_bus_config_t buscfg = {
        .sclk_io_num = SPI_BUS_SCK_GPIO,       // Clock line (GPIO 18)
        .mosi_io_num = SPI_BUS_MOSI_GPIO,      // MOSI line (GPIO 17)
        .miso_io_num = SPI_BUS_MISO_GPIO,      // MISO line (GPIO 8)
        .quadwp_io_num = -1,                   // Quad mode disabled
        .quadhd_io_num = -1,                   // Quad mode disabled
        // Max transfer size: 240x320 RGB565 display frame (153,600 bytes).
        // Shrink this if RAM is tight and you flush in smaller chunks.
        .max_transfer_sz = 240 * 320 * 2,
    };

    // Initialize the SPI host with automatic DMA channel selection
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

/**
 * Acquire exclusive access to the shared SPI bus.
 *
 * Blocks (waits) until the bus is available. Once acquired, the caller
 * has exclusive access until spi_bus_unlock() is called.
 *
 * MUST be called before any transaction or group of transactions on shared
 * bus devices (display, SD card, etc.).
 */
void spi_bus_lock(void)
{
    // In debug builds, configASSERT will fire if s_spi_mutex is NULL
    // (i.e., if spi_bus_shared_init was never called).
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "spi_bus_lock: Bus not initialized! Call spi_bus_shared_init() first.");
        return;  // Fail gracefully instead of hanging
    }

    // Take the semaphore (mutex). If another task holds it, wait forever.
    xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
}

/**
 * Release exclusive access to the shared SPI bus.
 *
 * Call this after spi_bus_lock(), once the operation (including any async
 * DMA transfer) is complete. Failing to call this will deadlock other tasks
 * waiting for the bus.
 */
void spi_bus_unlock(void)
{
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "spi_bus_unlock: Bus not initialized!");
        return;
    }

    xSemaphoreGive(s_spi_mutex);
}

