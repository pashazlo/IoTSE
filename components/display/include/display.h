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
#define DISPLAY_HEIGHT  170

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Инициализация дисплея ST7789 через esp_lcd
 * 
 * Автоматически инициализирует общую шину SPI (если она не была поднята)
 * и настраивает контроллер ST7789 в ландшафтную ориентацию (320x240).
 * 
 * @return ESP_OK при успехе, иначе код ошибки
 */
esp_err_t display_init(void);

/**
 * @brief Отрисовка буфера пикселей (Bitmap) на экран
 * 
 * Безопасно захватывает мьютекс шины SPI перед передачей данных,
 * исключая конфликты с SD-картой.
 * 
 * @param x0 Начальная координата X (0..319)
 * @param y0 Начальная координата Y (0..239)
 * @param x1 Конечная координата X (исключительно)
 * @param y1 Конечная координата Y (исключительно)
 * @param color_data Указатель на массив пикселей RGB565 (должен быть в DMA памяти)
 * 
 * @return ESP_OK при успехе
 */
esp_err_t display_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *color_data);

/**
 * @brief Заливка всего экрана одним цветом
 * 
 * @param color Цвет в формате RGB565
 * 
 * @return ESP_OK при успехе
 */
esp_err_t display_fill_color(uint16_t color);

/**
 * @brief Получить handle панели esp_lcd для продвинутого использования
 * 
 * @return Указатель на panel handle или NULL, если дисплей не инициализирован
 */
esp_lcd_panel_handle_t display_get_panel_handle(void);

#ifdef __cplusplus
}
#endif
