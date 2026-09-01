#include "ui.h"
#include "display.h"
#include "gfx_canvas.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UI";
// ===== Массив битмапа из Lopaka =====
static const uint8_t image_982f4ed7d64cf132d2647654eb5dff37_bits[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x[...]

// ============================================================================
// Функция отрисовки экрана (ИСПРАВЛЕННАЯ)
// ============================================================================

static void draw_screen_1(gfx_canvas_t *canvas) {
    // 1. Заливка фона РОЗОВЫМ
    gfx_canvas_fill(canvas, GFX_PINK);  // 0xD809
    
    // 2. Верхняя линия (статус бар) — отступ 13 пикселей
    gfx_canvas_draw_line(canvas, 0, 13, 319, 13, GFX_WHITE);
    
    // 3. Нижняя линия (подсказки) — отступ 218 пикселей
    gfx_canvas_draw_line(canvas, 0, 218, 319, 218, GFX_WHITE);
    
    // 4. Череп DEDsec — рисуем с правильными координатами
    //    Битмап 320x240, canvas 320x240
    //    Начинаем с (0, 14) чтобы не перекрывать верхнюю линию
    gfx_canvas_draw_bitmap_mono(canvas, 0, 14,
                                image_982f4ed7d64cf132d2647654eb5dff37_bits,
                                320, 226,  // 240 - 14 = 226 (высота видимой части)
                                GFX_WHITE);
}

// ============================================================================
// Задача UI
// ============================================================================

void ui_task(void *arg) {
    // 1. Создаём canvas (альбомная ориентация 320x240)
    gfx_canvas_t canvas;
    if (gfx_canvas_init(&canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate canvas");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Canvas allocated: %dx%d", canvas.width, canvas.height);
    
    // 2. Отрисовываем экран
    draw_screen_1(&canvas);
    
    // 3. Отправляем на дисплей
    esp_err_t err = gfx_canvas_flush(&canvas);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flush failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "UI rendered successfully!");
    }
    
    // 4. Основной цикл — здесь будут обновления UI
    while (1) {
        // Пока ничего не делаем, просто ждём
        // В будущем: читаем очереди, рисуем динамические элементы,
        // затем снова gfx_canvas_flush()
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    gfx_canvas_deinit(&canvas);
    vTaskDelete(NULL);
}
