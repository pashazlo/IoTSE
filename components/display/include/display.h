#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Display Configuration
// ============================================================================

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize ST7789 display via esp_lcd
 * 
 * Sets up SPI2 interface, initializes ST7789 controller,
 * configures orientation to landscape (320x240),
 * and enables backlight.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t display_init(void);

/**
 * @brief Draw bitmap to display
 * 
 * @param x0 Top-left X coordinate (0-319)
 * @param y0 Top-left Y coordinate (0-239)
 * @param x1 Bottom-right X coordinate (exclusive)
 * @param y1 Bottom-right Y coordinate (exclusive)
 * @param color_data RGB565 pixel data (must be DMA-capable memory)
 * 
 * @return ESP_OK on success
 */
esp_err_t display_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *color_data);

/**
 * @brief Fill entire screen with single color
 * 
 * @param color RGB565 color value
 * 
 * @return ESP_OK on success
 */
esp_err_t display_fill_color(uint16_t color);

/**
 * @brief Get esp_lcd panel handle for advanced usage
 * 
 * @return Panel handle or NULL if not initialized
 */
esp_lcd_panel_handle_t display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif
