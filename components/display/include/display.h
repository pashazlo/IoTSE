#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320

/**
 * @brief Initialize the ST7789 display: adds it as a device on the shared
 *        SPI bus (spi_bus_shared_init() MUST have been called already),
 *        resets/configures the panel and turns the backlight on.
 */
esp_err_t display_init(void);

/**
 * @brief Draw a rectangular region of RGB565 pixel data.
 *        Takes the shared SPI bus mutex, issues the DMA transfer, and
 *        blocks until the transfer has actually completed before
 *        releasing the mutex. Safe to call from any task, including
 *        ones sharing the bus with an SD card.
 *
 * @param x1,y1   top-left corner (inclusive)
 * @param x2,y2   bottom-right corner (exclusive)
 * @param color_data  RGB565 pixel buffer, (x2-x1)*(y2-y1) pixels,
 *                     caller keeps ownership and must not free/modify it
 *                     until this function returns.
 */
esp_err_t display_flush(int x1, int y1, int x2, int y2, const uint16_t *color_data);

/**
 * @brief Convenience helper: fill the whole screen with a single RGB565 color.
 */
esp_err_t display_fill_color(uint16_t color);

#ifdef __cplusplus
}
#endif
