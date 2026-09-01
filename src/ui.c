#include "ui.h"
#include "display.h"
#include "gfx_canvas.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UI";
// ===== Массив битмапа из Lopaka =====

// Отрисовка главного экрана
   static void draw_screen_1(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0xD809);

    // рамка по самому краю экрана
    gfx_canvas_draw_rect(canvas, 0, 0, 320, 170, 0xFFFF);

    // маленький квадрат 50×50 в левом верхнем углу
    gfx_canvas_fill_rect(canvas, 10, 20, 50, 50, 0xFFFF);

    // линия посередине
    gfx_canvas_draw_line(canvas, 0, 85, 319, 85, 0xFFFF);
}

// ============================================================================
// Задача UI
// ============================================================================
void ui_task(void *arg)
{
    gfx_canvas_t canvas;
    if (gfx_canvas_init(&canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate canvas");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Canvas: %dx%d", canvas.width, canvas.height);

    draw_screen_1(&canvas);

    esp_err_t err = gfx_canvas_flush(&canvas);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flush failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "UI rendered");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    gfx_canvas_deinit(&canvas);
    vTaskDelete(NULL);
}
