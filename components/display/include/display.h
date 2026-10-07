#pragma once

#include <stdint.h>
#include <stddef.h>
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
 * и настраивает контроллер ST7789 в ландшафтную ориентацию (320x170).
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
 * @param y0 Начальная координата Y (0..169)
 * @param x1 Конечная координата X (исключительно)
 * @param y1 Конечная координата Y (исключительно)
 * @param color_data Указатель на плотно упакованный массив пикселей RGB565.
 *                   DMA-память не требуется: данные копируются ping-pong
 *                   полосами внутри драйвера.
 * 
 * @return ESP_OK при успехе
 */
esp_err_t display_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *color_data);

/**
 * @brief Update one tightly packed RGB565 dirty rectangle.
 *
 * x2/y2 are exclusive, matching esp_lcd_panel_draw_bitmap(). The source
 * contains exactly (x2 - x1) * (y2 - y1) pixels in row-major order.
 */
esp_err_t display_update_rect(
    uint16_t x1,
    uint16_t y1,
    uint16_t x2,
    uint16_t y2,
    const uint16_t *pixel_data
);

/**
 * @brief Отрисовать RGB565-прямоугольник из буфера с шагом строки.
 *
 * Нужен canvas для отправки dirty rectangle без временного scratch-буфера.
 */
esp_err_t display_draw_bitmap_stride(
    int x0,
    int y0,
    int x1,
    int y1,
    const uint16_t *color_data,
    size_t source_stride_pixels
);

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
