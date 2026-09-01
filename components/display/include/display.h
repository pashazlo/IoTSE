#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Размеры в альбомном режиме (320x240)
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

// Публичные функции
esp_err_t display_init(void);
esp_err_t display_flush(int x1, int y1, int x2, int y2, const uint16_t *color_data);
esp_err_t display_fill_color(uint16_t color);
esp_err_t display_set_rotation(uint8_t rotation);  // 0=0°, 1=90°, 2=180°, 3=270°

// ============================================================================
// ТЕСТОВАЯ ФУНКЦИЯ — вызывай её отдельно из main() для проверки цветов
// ============================================================================

void display_test_colors(void);

#ifdef __cplusplus
}
#endif
