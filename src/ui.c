#include "ui.h"
#include "display.h"
#include "gfx_canvas.h"
#include "ibm_vga_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "UI";

// Очередь событий
static QueueHandle_t ui_queue = NULL;
#define UI_QUEUE_LEN 10

// Состояния экранов
typedef enum {
    UI_SCREEN_SPLASH = 0,
    UI_SCREEN_MAIN_MENU,
    UI_SCREEN_MODULE_APP // Состояние внутри выбранного модуля
} ui_screen_t;

static ui_screen_t current_screen = UI_SCREEN_SPLASH;

// ----- Структура и массив меню -----
typedef void (*menu_cb_t)(void);

typedef struct {
    const char *title;
    menu_cb_t callback;
} menu_item_t;

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
    {"Settings",     action_settings}
};

#define MENU_COUNT (sizeof(main_menu) / sizeof(main_menu[0]))
static int8_t current_selected = 0;

// ===== Отрисовка логотипа Red Team =====
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
    gfx_canvas_draw_line(canvas, cx - 68, cy,      cx - 46, cy,      0xFFFF);
    gfx_canvas_draw_line(canvas, cx + 46, cy,      cx + 68, cy,      0xFFFF);
    gfx_canvas_draw_line(canvas, cx,      cy - 68, cx,      cy - 46, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx,      cy + 46, cx,      cy + 68, 0xFFFF);

    // Небольшие засечки на концах прицела
    gfx_canvas_draw_line(canvas, cx - 68, cy - 4, cx - 68, cy + 4, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx + 68, cy - 4, cx + 68, cy + 4, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - 4, cy - 68, cx + 4, cy - 68, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - 4, cy + 68, cx + 4, cy + 68, 0xFFFF);
}

// Отрисовка сплэш-экрана (заставка)
static void draw_splash_screen(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, GFX_RGB565(0xF8, 0x00, 0x54));
    
    // верхняя и нижняя линии
    gfx_canvas_draw_line(canvas, 0, 12, DISPLAY_WIDTH - 1, 12, 0xFFFF);
    gfx_canvas_draw_line(canvas, 0, DISPLAY_HEIGHT - 12, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 12, 0xFFFF);

    // логотип справа
    draw_redteam_logo(canvas, 230, DISPLAY_HEIGHT / 2); 
}

// Отрисовка главного меню
static void draw_main_menu(gfx_canvas_t *canvas)
{
    // Черный фон для удобного чтения меню
    gfx_canvas_fill(canvas, 0x0000);

    // Заголовок
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, GFX_RGB565(0xF8, 0x00, 0x54));

    // Вывод элементов меню
    int16_t start_y = 28;
    int16_t line_height = 20;

    for (uint8_t i = 0; i < MENU_COUNT; i++) {
        int16_t y = start_y + (i * line_height);

        if (i == current_selected) {
            // Подсветка выбранного пункта прямоугольником
            gfx_canvas_fill_rect(canvas, 5, y - 2, 180, line_height - 2, GFX_RGB565(0xF8, 0x00, 0x54));
            // Отрисовка текста (белый текст на красном фоне)
            // gfx_canvas_draw_str(10, y, main_menu[i].title, &Px437_IBM_VGA_8x14_2x8pt7b, 0xFFFF);
        } else {
            // Неактивный пункт меню
            // gfx_canvas_draw_str(10, y, main_menu[i].title, &Px437_IBM_VGA_8x14_2x8pt7b, 0x8410);
        }
    }

    // Мини-логотип справа в меню для красоты
    draw_redteam_logo(canvas, 260, 90);
}

// Единый диспетчер отрисовки
static void ui_render(gfx_canvas_t *canvas)
{
    switch (current_screen) {
        case UI_SCREEN_SPLASH:
            draw_splash_screen(canvas);
            break;
        case UI_SCREEN_MAIN_MENU:
            draw_main_menu(canvas);
            break;
        default:
            break;
    }
    gfx_canvas_flush(canvas);
}

// Потокобезопасные функции отправки событий
BaseType_t ui_send_event(ui_event_t evt) {
    if (ui_queue == NULL) return pdFAIL;
    return xQueueSend(ui_queue, &evt, 0);
}

BaseType_t ui_send_event_from_isr(ui_event_t evt, BaseType_t *hp_task_woken) {
    if (ui_queue == NULL) return pdFAIL;
    return xQueueSendFromISR(ui_queue, &evt, hp_task_woken);
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

    ui_queue = xQueueCreate(UI_QUEUE_LEN, sizeof(ui_event_t));
    if (ui_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UI queue");
        gfx_canvas_deinit(&canvas);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Canvas initialized: %dx%d", canvas.width, canvas.height);

    // 1. Показываем сплэш-экран
    current_screen = UI_SCREEN_SPLASH;
    ui_render(&canvas);

    // Задержка 2 секунды перед переходом в меню
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 2. Переходим в главное меню
    current_screen = UI_SCREEN_MAIN_MENU;
    ui_render(&canvas);

    ui_event_t evt;

    // Главный событийный цикл задачи (0% CPU во время ожидания)
    while (1) {
        if (xQueueReceive(ui_queue, &evt, portMAX_DELAY) == pdTRUE) {
            
            if (current_screen == UI_SCREEN_MAIN_MENU) {
                switch (evt) {
                    case UI_EVT_UP:
                        current_selected--;
                        if (current_selected < 0) current_selected = MENU_COUNT - 1;
                        ui_render(&canvas);
                        break;

                    case UI_EVT_DOWN:
                        current_selected++;
                        if (current_selected >= MENU_COUNT) current_selected = 0;
                        ui_render(&canvas);
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
        }
    }

    gfx_canvas_deinit(&canvas);
    vTaskDelete(NULL);
}
