#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// SPI Bus Configuration
// ============================================================================

// SPI host used for the shared bus (display + SD card)
#define SHARED_SPI_HOST   SPI2_HOST

// Physical bus pins (shared by ALL devices on this bus)
#define SPI_BUS_SCK_GPIO   18      // Clock — слушают ВСЕ устройства
#define SPI_BUS_MOSI_GPIO  17      // Data Out (Master→Slaves) — слушают ВСЕ
#define SPI_BUS_MISO_GPIO  8       // Data In (Slaves→Master) — подключи SD картовод сюда!

// NOTE: Каждое устройство (Display, SD card) имеет свой CS (Chip Select):
// - Display: DISP_CS_GPIO = 7 (в components/display/display.c)
// - SD card: SD_CS_GPIO = 9 (будет добавлено в компонент sd_card)
// SCK и MOSI общие, но мьютекс (см. ниже) гарантирует синхронизацию операций.

/**
 * @brief Initialize the physical SPI bus (once, before any device is added).
 *        Also creates the mutex that MUST be taken before any transaction
 *        sequence on this bus (display flush, SD card read/write, etc).
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if semaphore allocation fails,
 *         or error from spi_bus_initialize().
 *
 * Safe to call once from app_main() before any device driver init.
 * Subsequent calls are no-op (protected by s_bus_initialized flag).
 */
esp_err_t spi_bus_shared_init(void);

/**
 * @brief Acquire exclusive logical access to the shared SPI bus.
 *        Blocks until available. Use around any multi-step transaction
 *        sequence (e.g. a full display flush, or an SD card read/write).
 *
 * IMPORTANT: Must always be paired with spi_bus_unlock().
 *            Failing to unlock will deadlock all other bus consumers.
 *
 * Example:
 *   spi_bus_lock();
 *   display_flush(...);
 *   spi_bus_unlock();
 */
void spi_bus_lock(void);

/**
 * @brief Release exclusive logical access to the shared SPI bus.
 *        Must be called after spi_bus_lock(), once the operation
 *        (including any async DMA transfer) has actually completed.
 *
 * IMPORTANT: Only call if spi_bus_lock() succeeded (or was not attempted).
 *            Unmatched unlock() will cause semaphore corruption.
 */
void spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
