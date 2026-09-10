#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// SPI host used for the shared bus (display + SD card)
#define SHARED_SPI_HOST   SPI2_HOST

// Physical bus pins (shared by ALL devices on this bus)
#define SPI_BUS_SCK_GPIO   18
#define SPI_BUS_MOSI_GPIO  17
#define SPI_BUS_MISO_GPIO  -1   // set to a real GPIO once SD card is wired in

/**
 * @brief Initialize the physical SPI bus (once, before any device is added).
 *        Also creates the mutex that MUST be taken before any transaction
 *        sequence on this bus (display flush, SD card read/write, etc).
 *
 * Safe to call once from app_main() before any device driver init.
 */
esp_err_t spi_bus_shared_init(void);

/**
 * @brief Acquire exclusive logical access to the shared SPI bus.
 *        Blocks until available. Use around any multi-step transaction
 *        sequence (e.g. a full display flush, or an SD card read/write).
 */
void spi_bus_lock(void);

/**
 * @brief Release exclusive logical access to the shared SPI bus.
 *        Must be called after spi_bus_lock(), once the operation
 *        (including any async DMA transfer) has actually completed.
 */
void spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
