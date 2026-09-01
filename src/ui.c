#include "ui.h"
#include "display.h"
#include "gfx_canvas.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UI";
// ===== Массив битмапа из Lopaka =====
static void draw_redteam_logo(gfx_canvas_t *canvas, int16_t cx, int16_t cy)
{
    // Внешний круг (глаз)
    gfx_canvas_draw_circle(canvas, cx, cy, 42, 0xFFFF);
    gfx_canvas_draw_circle(canvas, cx, cy, 41, 0xFFFF);   // толщина

    // Средний круг (радужка)
    gfx_canvas_draw_circle(canvas, cx, cy, 26, 0xFFFF);

    // Зрачок
    gfx_canvas_fill_circle(canvas, cx, cy, 11, 0xFFFF);

    // Блик
    gfx_canvas_fill_circle(canvas, cx - 5, cy - 5, 3, GFX_RGB565(0xF8, 0x00, 0x54));

    // Прицел (4 линии)
    gfx_canvas_draw_line(canvas, cx - 68, cy,     cx - 46, cy,     0xFFFF);
    gfx_canvas_draw_line(canvas, cx + 46, cy,     cx + 68, cy,     0xFFFF);
    gfx_canvas_draw_line(canvas, cx,     cy - 68, cx,     cy - 46, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx,     cy + 46, cx,     cy + 68, 0xFFFF);

    // Небольшие засечки на концах прицела
    gfx_canvas_draw_line(canvas, cx - 68, cy - 4, cx - 68, cy + 4, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx + 68, cy - 4, cx + 68, cy + 4, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - 4, cy - 68, cx + 4, cy - 68, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - 4, cy + 68, cx + 4, cy + 68, 0xFFFF);
}
// Отрисовка главного экрана
 static void draw_screen_1(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, GFX_RGB565(0xF8, 0x00, 0x54));
// верхняя и нижняя линии
    gfx_canvas_draw_line(canvas, 0, 12, 319, 12, 0xFFFF);
    gfx_canvas_draw_line(canvas, 0, 158, 319, 158, 0xFFFF);

    // логотип справа
    draw_redteam_logo(canvas, 230, 85); 
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
