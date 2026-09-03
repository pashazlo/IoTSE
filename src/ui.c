#include "ui.h"
#include "display.h"
#include "gfx_canvas.h"
#include "assets/ibm_vga_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "UI";
#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)   // короткий алиас на длинное имя шрифта

static QueueHandle_t ui_queue = NULL;
#define UI_QUEUE_LEN 10

typedef enum { UI_SCREEN_SPLASH = 0, UI_SCREEN_MAIN_MENU, UI_SCREEN_MODULE_APP } ui_screen_t;
static ui_screen_t current_screen = UI_SCREEN_SPLASH;

typedef void (*menu_cb_t)(void);
typedef struct { const char *title; menu_cb_t callback; } menu_item_t;

static void action_ir(void)       { ESP_LOGI(TAG, "Opened IR"); }
static void action_settings(void) { ESP_LOGI(TAG, "Opened Settings"); }
static void action_rf(void)       { ESP_LOGI(TAG, "Opened RF (Sub-GHz)"); }
static void action_nrf(void)      { ESP_LOGI(TAG, "Opened NRF24"); }
static void action_wifi(void)     { ESP_LOGI(TAG, "Opened Wi-Fi"); }
static void action_bt(void)       { ESP_LOGI(TAG, "Opened Bluetooth"); }

static const menu_item_t main_menu[] = {
    {"IR Remote",    action_ir},
    {"RF (Sub-GHz)", action_rf},
    {"NRF24",        action_nrf},
    {"Wi-Fi",        action_wifi},
    {"Bluetooth",    action_bt},
    {"Settings",     action_settings},
};
#define MENU_COUNT (sizeof(main_menu) / sizeof(main_menu[0]))
static int8_t current_selected = 0;

// Отрисовка логотипа Red Team (глаз + прицел)
static void draw_redteam_logo(gfx_canvas_t *canvas, int16_t cx, int16_t cy, uint16_t bg_color)
{
    gfx_canvas_draw_circle(canvas, cx, cy, 42, 0xFFFF);
    gfx_canvas_draw_circle(canvas, cx, cy, 41, 0xFFFF);   // толщина обода
    gfx_canvas_draw_circle(canvas, cx, cy, 26, 0xFFFF);   // радужка
    gfx_canvas_fill_circle(canvas, cx, cy, 11, 0xFFFF);   // зрачок
    gfx_canvas_fill_circle(canvas, cx - 5, cy - 5, 3, bg_color); // блик

    // Прицел: 4 луча + засечки на концах
    const int16_t arm = 68, gap = 46, tick = 4;
    gfx_canvas_draw_line(canvas, cx - arm, cy, cx - gap, cy, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx + gap, cy, cx + arm, cy, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx, cy - arm, cx, cy - gap, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx, cy + gap, cx, cy + arm, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - arm, cy - tick, cx - arm, cy + tick, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx + arm, cy - tick, cx + arm, cy + tick, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - tick, cy - arm, cx + tick, cy - arm, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - tick, cy + arm, cx + tick, cy + arm, 0xFFFF);
}

static void draw_splash_screen(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, GFX_RGB565(0xF8, 0x00, 0x54));
    gfx_canvas_draw_line(canvas, 0, 12, DISPLAY_WIDTH - 1, 12, 0xFFFF);
    gfx_canvas_draw_line(canvas, 0, DISPLAY_HEIGHT - 12, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 12, 0xFFFF);
    draw_redteam_logo(canvas, 230, DISPLAY_HEIGHT / 2, GFX_RGB565(0xF8, 0x00, 0x54));
}

static void draw_main_menu(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, 0xFFFF);

    const int16_t start_y = 40;
    const int16_t line_h  = 20;

    for (uint8_t i = 0; i < MENU_COUNT; i++) {
        int16_t y = start_y + (i * line_h);
        bool active = (i == current_selected);

        if (active) {
            // Выбранный пункт: [Текст]
            char buf[32];
            snprintf(buf, sizeof(buf), "[%s]", main_menu[i].title);
            gfx_canvas_draw_str(canvas, 10, y, buf, UI_FONT, 0xFFFF);
        } else {
            // Обычный пункт
            gfx_canvas_draw_str(canvas, 10, y, main_menu[i].title, UI_FONT, 0x8410);
        }
    }

    draw_redteam_logo(canvas, 220, 90, 0x0000);
}

static void ui_render(gfx_canvas_t *canvas)
{
    switch (current_screen) {
        case UI_SCREEN_SPLASH:     draw_splash_screen(canvas); break;
        case UI_SCREEN_MAIN_MENU:  draw_main_menu(canvas);     break;
        default: break;
    }
    gfx_canvas_flush(canvas);
}

BaseType_t ui_send_event(ui_event_t evt) {
    return ui_queue ? xQueueSend(ui_queue, &evt, 0) : pdFAIL;
}

BaseType_t ui_send_event_from_isr(ui_event_t evt, BaseType_t *hp_task_woken) {
    return ui_queue ? xQueueSendFromISR(ui_queue, &evt, hp_task_woken) : pdFAIL;
}

void ui_task(void *arg)
{
    gfx_canvas_t canvas;
    if (gfx_canvas_init(&canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate canvas");
        vTaskDelete(NULL);
        return;
    }

    ui_queue = xQueueCreate(UI_QUEUE_LEN, sizeof(ui_event_t));
    if (ui_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UI queue");
        gfx_canvas_deinit(&canvas);
        vTaskDelete(NULL);
        return;
    }

    current_screen = UI_SCREEN_SPLASH;
    ui_render(&canvas);
    vTaskDelay(pdMS_TO_TICKS(2000));

    current_screen = UI_SCREEN_MAIN_MENU;
    ui_render(&canvas);

    ui_event_t evt;
    while (1) {
        if (xQueueReceive(ui_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (current_screen != UI_SCREEN_MAIN_MENU) continue;

      switch (evt) {

    case UI_EVT_UP:
        current_selected = (current_selected <= 0)
            ? MENU_COUNT - 1
            : current_selected - 1;

        ui_render(&canvas);
        break;

    case UI_EVT_DOWN:
        current_selected = (current_selected >= (int8_t)MENU_COUNT - 1)
            ? 0
            : current_selected + 1;

        ui_render(&canvas);
        break;

    case UI_EVT_LEFT:
        ESP_LOGI(TAG, "UI LEFT");
        break;

    case UI_EVT_RIGHT:
        ESP_LOGI(TAG, "UI RIGHT");
        break;
          
    case UI_EVT_SELECT:
        if (main_menu[current_selected].callback) {
            main_menu[current_selected].callback();
        }
        break;
          
    default:
        break;
}
    }

    gfx_canvas_deinit(&canvas);
    vTaskDelete(NULL);
}
