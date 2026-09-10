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
 * @brief Acquire exclusive logical access to the shared SPI bus,
 *        with a bounded timeout instead of waiting forever.
 *
 * ВАЖНО (изменение относительно старой версии): раньше эта функция
 * ждала portMAX_DELAY — если где-то в коде был непарный lock()/unlock(),
 * прошивка зависала НАВСЕГДА и МОЛЧА, без единой строки в логе.
 * Теперь при неудаче функция гарантированно возвращает управление
 * с логом ошибки — баг станет видимым таймаутом в логе, а не тихим
 * зависанием устройства.
 *
 * @param timeout  максимальное время ожидания, например pdMS_TO_TICKS(1000).
 *                 portMAX_DELAY тоже допустим, но НЕ рекомендуется для
 *                 новых мест использования — именно он был первопричиной
 *                 тихих зависаний.
 *
 * @return true  — шина захвачена; ОБЯЗАТЕЛЬНО вызвать spi_bus_unlock()
 *                 после операции.
 *         false — не удалось захватить за отведённое время; шина НЕ
 *                 захвачена, spi_bus_unlock() звать НЕ нужно.
 *
 * Example:
 *   if (spi_bus_lock(pdMS_TO_TICKS(1000))) {
 *       display_flush(...);
 *       spi_bus_unlock();
 *   } else {
 *       // шина занята дольше секунды — что-то не так, обработать ошибку
 *   }
 */
bool spi_bus_lock(TickType_t timeout);

/**
 * @brief Release exclusive logical access to the shared SPI bus.
 *        Call ONLY if spi_bus_lock() returned true.
 *
 * IMPORTANT: Unmatched unlock() (без предшествующего успешного lock())
 *            залогируется как ошибка — это симптом бага в вызывающем коде.
 */
void spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
